#include <vehicle_thread.h>
#include "app_runtime.h"
#include "IPC/navigation_ipc_runtime.h"
#include "IPC/yolo_safety_runtime.h"
#include "Vehicle/adapters/rtos/vehicle_command_mailbox.h"
#include "Vehicle/application/vehicle_service.h"
#include "Vehicle/platform/fsp_vehicle_factory.h"
#include "Vehicle/platform/fsp_vehicle_i2c.h"
#include "SEGGER_RTT/bsp_print.h"

#define VEHICLE_CONTROL_PERIOD_MS          (10U)
#define VEHICLE_NAV_COMMAND_TIMEOUT_MS     (500U) /* M85导航失联停车时间。 */
#define VEHICLE_STARTUP_SUCTION_PERCENT    (80U)

/*
 * 无IMU期间用“转向PWM百分比×时间”估算连续单向转角。
 * 675 ms@100%按现有参数等效为约90度，270000 %-ms约等于一整圈，实车后必须重标定。
 */
#define VEHICLE_NAV_TURN_90_TIME_MS             (675U)
#define VEHICLE_NAV_TURN_CALIBRATION_PERCENT    (100U)
#define VEHICLE_NAV_TURN_360_EFFORT_LIMIT       \
    (4U * VEHICLE_NAV_TURN_90_TIME_MS * VEHICLE_NAV_TURN_CALIBRATION_PERCENT)

/* 置0仅关闭YOLO对电机的安全仲裁，YOLO IPC结果仍会被读取并排空。 */
#define VEHICLE_YOLO_SAFETY_ENABLE              (1U)

/* YOLO只识别一种目标，仅采用最近6个不同源帧结果的3票仲裁。 */
#define VEHICLE_YOLO_VOTE_WINDOW_RESULTS         (6U)
#define VEHICLE_YOLO_REQUIRED_POSITIVES          (3U)

#if (VEHICLE_YOLO_SAFETY_ENABLE > 1U)
#error "VEHICLE_YOLO_SAFETY_ENABLE must be 0 or 1"
#endif

#if (VEHICLE_YOLO_REQUIRED_POSITIVES == 0U) || \
    (VEHICLE_YOLO_REQUIRED_POSITIVES > VEHICLE_YOLO_VOTE_WINDOW_RESULTS)
#error "YOLO vote configuration is invalid"
#endif

#define VEHICLE_NAV_MOTOR_OUTPUT_ENABLE    (1U)

/*
 * 车轮直行验证开关：
 *   1：上电完成初始化后，以 90% 速度直行 3 秒并自动停车；
 *   0：关闭上电自检，直接进入正常遥控流程。
 *
 * 首次测试时请架空车轮。确认左右轮方向正确后，再把车辆放到地面测试。
 */
#define VEHICLE_WHEEL_STRAIGHT_TEST_ENABLE       (0U)
#define VEHICLE_WHEEL_TEST_START_DELAY_MS        (2500U)
#define VEHICLE_WHEEL_TEST_RUN_TIME_MS           (3000U)
#define VEHICLE_WHEEL_TEST_SPEED_PERCENT         (90U)

typedef enum e_vehicle_wheel_test_state
{
    VEHICLE_WHEEL_TEST_WAITING = 0,
    VEHICLE_WHEEL_TEST_RUNNING,
    VEHICLE_WHEEL_TEST_COMPLETE,
} vehicle_wheel_test_state_t;

typedef struct st_vehicle_yolo_vote
{
    bool detected;
} vehicle_yolo_vote_t;

typedef struct st_vehicle_yolo_guard
{
    vehicle_yolo_vote_t votes[VEHICLE_YOLO_VOTE_WINDOW_RESULTS];
    uint32_t vote_count;
    bool have_sequence;
    uint8_t last_sequence;
} vehicle_yolo_guard_t;

static vehicle_result_t execute_command(vehicle_command_t const * command)
{
    switch (command->kind)
    {
        case VEHICLE_COMMAND_MANUAL:
#if !VEHICLE_NAV_MOTOR_OUTPUT_ENABLE
            if((VEHICLE_COMMAND_SOURCE_IPC == command->source) &&
               (VEHICLE_MANUAL_STOP != command->manual_action))
            {
                return vehicle_service_manual_command(VEHICLE_MANUAL_STOP, 0U);
            }
#endif
            if(VEHICLE_COMMAND_SOURCE_IPC == command->source)
            {
                return vehicle_service_navigation_command(command->manual_action,
                                                          command->speed_percent);
            }
            return vehicle_service_manual_command(command->manual_action,
                                                  command->speed_percent);
        case VEHICLE_COMMAND_SET_SUCTION:
            return vehicle_service_suction_set(command->suction_enable,
                                               command->suction_percent);
        case VEHICLE_COMMAND_SET_SPEED:
            return vehicle_service_automatic_speed_set(command->speed_percent);
        case VEHICLE_COMMAND_START_AUTOMATIC:
        {
            vehicle_result_t const speed_result =
                vehicle_service_automatic_speed_set(command->speed_percent);
            return (VEHICLE_RESULT_OK == speed_result) ?
                   vehicle_service_automatic_start() : speed_result;
        }
        case VEHICLE_COMMAND_SET_MODE:
            return vehicle_service_mode_set(command->mode);
        case VEHICLE_COMMAND_NAVIGATION_MANUAL_LATCH:
            return vehicle_service_mode_set(VEHICLE_MODE_MANUAL);
        case VEHICLE_COMMAND_NAVIGATION_REARMED_STOP:
            return vehicle_service_navigation_command(VEHICLE_MANUAL_STOP, 0U);
        case VEHICLE_COMMAND_EMERGENCY_STOP:
            vehicle_service_emergency_stop();
            return VEHICLE_RESULT_OK;
        default:
            return VEHICLE_RESULT_INVALID_ARGUMENT;
    }
}

static bool command_requests_auto(vehicle_command_t const * command)
{
    return (VEHICLE_COMMAND_SOURCE_NRF == command->source) &&
           ((VEHICLE_COMMAND_START_AUTOMATIC == command->kind) ||
            ((VEHICLE_COMMAND_SET_MODE == command->kind) &&
             (VEHICLE_MODE_AUTOMATIC == command->mode)));
}

static bool navigation_sequence_is_newer(uint32_t sequence, uint8_t reference)
{
    uint8_t const delta = (uint8_t) ((uint8_t) sequence - reference);
    return (0U != delta) && (delta < 128U);
}

static void vehicle_yolo_guard_reset(vehicle_yolo_guard_t * p_guard)
{
    p_guard->vote_count = 0U;
    p_guard->have_sequence = false;
    p_guard->last_sequence = 0U;
}

static bool vehicle_yolo_guard_result_add(vehicle_yolo_guard_t * p_guard,
                                          yolo_safety_result_t const * p_result)
{
    if(p_guard->have_sequence &&
       !navigation_sequence_is_newer(p_result->frame_sequence,
                                     p_guard->last_sequence))
    {
        return false;
    }

    p_guard->have_sequence = true;
    p_guard->last_sequence = p_result->frame_sequence;

    if(p_guard->vote_count >= VEHICLE_YOLO_VOTE_WINDOW_RESULTS)
    {
        for(uint32_t index = 1U; index < p_guard->vote_count; index++)
        {
            p_guard->votes[index - 1U] = p_guard->votes[index];
        }
        p_guard->vote_count--;
    }

    p_guard->votes[p_guard->vote_count].detected = p_result->detected;
    p_guard->vote_count++;

    uint32_t positive_count = 0U;
    for(uint32_t index = 0U; index < p_guard->vote_count; index++)
    {
        positive_count += p_guard->votes[index].detected ? 1U : 0U;
    }
    return positive_count >= VEHICLE_YOLO_REQUIRED_POSITIVES;
}

/* 安全锁定后的AUTO只重新进入审核状态，不允许内置自动轨迹直接驱动车轮。 */
static vehicle_result_t execute_rearm_auto_command(vehicle_command_t const * command)
{
    vehicle_result_t const speed_result =
        vehicle_service_automatic_speed_set(command->speed_percent);
    return (VEHICLE_RESULT_OK == speed_result) ?
           vehicle_service_mode_set(VEHICLE_MODE_AUTOMATIC) : speed_result;
}

static bool command_requests_stop(vehicle_command_t const * command)
{
    return (VEHICLE_COMMAND_EMERGENCY_STOP == command->kind) ||
           (VEHICLE_COMMAND_NAVIGATION_MANUAL_LATCH == command->kind) ||
           (VEHICLE_COMMAND_NAVIGATION_REARMED_STOP == command->kind) ||
           ((VEHICLE_COMMAND_MANUAL == command->kind) &&
             (VEHICLE_MANUAL_STOP == command->manual_action));
}

static void log_i2c_error_if_changed(void)
{
    static bool previous_valid;
    static fsp_vehicle_i2c_error_snapshot_t previous;
    fsp_vehicle_i2c_error_snapshot_t current;

    if (!fsp_vehicle_i2c_error_snapshot_get(&current)) return;

    bool const changed = (!previous_valid) ||
                         (current.stage != previous.stage) ||
                         (current.fsp_error != previous.fsp_error) ||
                         (current.expected_event != previous.expected_event) ||
                         (current.actual_event != previous.actual_event) ||
                         (current.timed_out != previous.timed_out);
    if (changed)
    {
        g_printf("[VEHICLE][I2C ERR] count=%lu stage=%u fsp=%d timeout=%u "
                 "expected=%u actual=%u addr=0x%02X reg=0x%02X len=%lu.\r\n",
                 (unsigned long) current.error_count,
                 (unsigned int) current.stage,
                 (int) current.fsp_error,
                 (unsigned int) current.timed_out,
                 (unsigned int) current.expected_event,
                 (unsigned int) current.actual_event,
                 (unsigned int) current.slave_address,
                 (unsigned int) current.register_address,
                 (unsigned long) current.transfer_length);
    }
    previous = current;
    previous_valid = true;
}

/**
 * @brief 底盘硬件的唯一所有者。
 *
 * Command RX 等输入线程只向 mailbox 提交目标，本线程在固定 10 ms 周期中执行目标、
 * 读取 IMU、运行控制器并写 GPT，从结构上避免多个线程同时操作电机。
 */
void vehicle_thread_entry(void * pvParameters)
{
    vehicle_dependencies_t dependencies;
    vehicle_command_t command;
    vehicle_result_t init_result;
    TickType_t wake_tick;
    TickType_t automatic_enter_tick;
    TickType_t last_navigation_tick;
    TickType_t navigation_guard_tick;
    TickType_t safety_latch_tick = 0U;
    bool navigation_motion_active = false;
    bool safety_manual_latched = false;
    bool awaiting_navigation_rearm = false;
    vehicle_mode_t control_mode = VEHICLE_MODE_MANUAL;
    vehicle_manual_command_t navigation_turn_direction = VEHICLE_MANUAL_STOP;
    uint8_t navigation_turn_speed_percent = 0U;
    uint8_t safety_latch_navigation_sequence = 0U;
    uint8_t last_navigation_sequence = 0U;
    uint32_t navigation_turn_effort = 0U;
    vehicle_yolo_guard_t yolo_guard;
    vehicle_result_t last_command_error = VEHICLE_RESULT_OK;
#if VEHICLE_WHEEL_STRAIGHT_TEST_ENABLE
    vehicle_wheel_test_state_t wheel_test_state = VEHICLE_WHEEL_TEST_WAITING;
    TickType_t wheel_test_tick;
#endif

    FSP_PARAMETER_NOT_USED(pvParameters);

    if(!app_runtime_init())
    {
        g_printf("[SYSTEM][FATAL] runtime EventGroup/timer initialization failed.\r\n");
        vTaskSuspend(NULL);
    }

    if(!vehicle_command_mailbox_init())
    {
        vehicle_service_emergency_stop();
        g_printf("[VEHICLE][FATAL] command mailbox initialization failed.\r\n");
        vTaskSuspend(NULL);
    }

    if(!fsp_vehicle_dependencies_create(&dependencies))
    {
        vehicle_service_emergency_stop();
        g_printf("[VEHICLE][FATAL] dependency initialization failed; check IIC0 open.\r\n");
        vTaskSuspend(NULL);
    }

    init_result = vehicle_service_init(&dependencies);
    if(VEHICLE_RESULT_OK != init_result)
    {
        vehicle_service_emergency_stop();
        if(VEHICLE_RESULT_SENSOR_ERROR == init_result)
        {
            log_i2c_error_if_changed();
        }
        g_printf("[VEHICLE][FATAL] service initialization failed: %u.\r\n",
                 (unsigned int) init_result);
        vTaskSuspend(NULL);
    }

    g_printf("[VEHICLE] safe outputs established; waiting for startup gate.\r\n");
    app_runtime_wait_for_start();
    g_printf("[VEHICLE] startup gate released.\r\n");

    init_result = vehicle_service_suction_set(true,
                                              VEHICLE_STARTUP_SUCTION_PERCENT);
    if(VEHICLE_RESULT_OK != init_result)
    {
        vehicle_service_emergency_stop();
        g_printf("[VEHICLE][FATAL] startup suction failed: %u.\r\n",
                 (unsigned int) init_result);
        vTaskSuspend(NULL);
    }
    g_printf("[VEHICLE] suction starting at %u%%.\r\n",
             (unsigned int) VEHICLE_STARTUP_SUCTION_PERCENT);
#if VEHICLE_NAV_MOTOR_OUTPUT_ENABLE
    g_printf("[VEHICLE] navigation motor output ENABLED.\r\n");
#else
    g_printf("[VEHICLE] navigation motor output DISABLED; IPC log-only.\r\n");
#endif
#if !VEHICLE_MPU6050_ENABLE
    g_printf("[VEHICLE][WARN] MPU6050 isolated; straight motion is open-loop.\r\n");
#endif

    wake_tick = xTaskGetTickCount();
    automatic_enter_tick = wake_tick;
    last_navigation_tick = wake_tick;
    navigation_guard_tick = wake_tick;
    vehicle_yolo_guard_reset(&yolo_guard);
#if VEHICLE_WHEEL_STRAIGHT_TEST_ENABLE
    wheel_test_tick = wake_tick;
    g_printf("[VEHICLE][TEST] wheel test waiting for suction\r\n");
#endif
    for (;;)
    {
        TickType_t const guard_now = xTaskGetTickCount();
        TickType_t const guard_elapsed_ticks = guard_now - navigation_guard_tick;
        navigation_guard_tick = guard_now;

        if(navigation_motion_active &&
           ((VEHICLE_MANUAL_TURN_LEFT == navigation_turn_direction) ||
            (VEHICLE_MANUAL_TURN_RIGHT == navigation_turn_direction)))
        {
            uint32_t const elapsed_ms = (uint32_t)
                (((uint64_t) guard_elapsed_ticks * 1000U) / configTICK_RATE_HZ);
            uint32_t const effort_increment =
                (uint32_t) navigation_turn_speed_percent * elapsed_ms;
            navigation_turn_effort =
                (navigation_turn_effort > (UINT32_MAX - effort_increment)) ?
                UINT32_MAX : navigation_turn_effort + effort_increment;

            if(navigation_turn_effort >= VEHICLE_NAV_TURN_360_EFFORT_LIMIT)
            {
                vehicle_result_t const latch_result =
                    vehicle_service_mode_set(VEHICLE_MODE_MANUAL);
                if(VEHICLE_RESULT_OK == latch_result)
                {
                    control_mode = VEHICLE_MODE_MANUAL;
                    safety_manual_latched = true;
                    awaiting_navigation_rearm = false;
                    safety_latch_tick = guard_now;
                    safety_latch_navigation_sequence = last_navigation_sequence;
                    navigation_motion_active = false;
                    navigation_turn_direction = VEHICLE_MANUAL_STOP;
                    g_printf("[VEHICLE][SAFE] turn effort reached 360-degree guard; "
                             "manual latch entered.\r\n");
                }
                else
                {
                    vehicle_service_emergency_stop();
                }
            }
        }

        yolo_safety_result_t yolo_result;
        while(yolo_safety_result_take(&yolo_result))
        {
            if((0U != VEHICLE_YOLO_SAFETY_ENABLE) &&
               (VEHICLE_MODE_AUTOMATIC == control_mode) &&
               !safety_manual_latched &&
               vehicle_yolo_guard_result_add(&yolo_guard, &yolo_result))
            {
                vehicle_result_t const latch_result =
                    vehicle_service_mode_set(VEHICLE_MODE_MANUAL);
                if(VEHICLE_RESULT_OK == latch_result)
                {
                    control_mode = VEHICLE_MODE_MANUAL;
                    safety_manual_latched = true;
                    awaiting_navigation_rearm = false;
                    safety_latch_tick = xTaskGetTickCount();
                    safety_latch_navigation_sequence = last_navigation_sequence;
                    navigation_motion_active = false;
                    navigation_turn_direction = VEHICLE_MANUAL_STOP;
                    navigation_turn_speed_percent = 0U;
                    navigation_turn_effort = 0U;
                    g_printf("[VEHICLE][SAFE] YOLO 3-of-6 vote triggered; "
                             "manual latch entered.\r\n");
                }
                else
                {
                    vehicle_service_emergency_stop();
                }
            }
        }

        if (vehicle_command_mailbox_take(&command))
        {
#if VEHICLE_WHEEL_STRAIGHT_TEST_ENABLE
            /* 自检期间接受急停和普通停车，确保手持控制器始终可以停止车轮。 */
            if ((VEHICLE_WHEEL_TEST_COMPLETE == wheel_test_state) ||
                command_requests_stop(&command))
#endif
            {
                bool const mode_command = VEHICLE_COMMAND_SET_MODE == command.kind;
                bool const auto_start_command =
                    VEHICLE_COMMAND_START_AUTOMATIC == command.kind;
                bool const auto_request = command_requests_auto(&command);
                bool const auto_received_after_latch =
                    ((int32_t) (command.received_tick - safety_latch_tick) > 0);
                bool const rearm_request = safety_manual_latched &&
                    auto_request && auto_received_after_latch;
                bool const stale_auto_blocked = safety_manual_latched &&
                    auto_request && !rearm_request;
                bool const command_allowed = !stale_auto_blocked && (mode_command ||
                    (VEHICLE_COMMAND_MANUAL != command.kind) ||
                    (VEHICLE_MODE_MANUAL == control_mode));
                vehicle_result_t const command_result = command_allowed ?
                    (rearm_request ? execute_rearm_auto_command(&command) :
                                     execute_command(&command)) :
                    VEHICLE_RESULT_OK;
                if(VEHICLE_RESULT_OK != command_result)
                {
                    if(command_result != last_command_error)
                    {
                        vehicle_status_t status;
                        vehicle_service_status_get(&status);
                        g_printf("[VEHICLE][CMD ERR] result=%u source=%u kind=%u action=%u "
                                 "speed=%u suction_ready=%u heading=%u.\r\n",
                                 (unsigned int) command_result,
                                 (unsigned int) command.source,
                                 (unsigned int) command.kind,
                                 (unsigned int) command.manual_action,
                                 (unsigned int) command.speed_percent,
                                 (unsigned int) status.suction_ready,
                                 (unsigned int) status.heading_enabled);
                    }
                    last_command_error = command_result;
                }
                else if(VEHICLE_RESULT_OK != last_command_error)
                {
                    g_printf("[VEHICLE] command execution recovered.\r\n");
                    last_command_error = VEHICLE_RESULT_OK;
                }
                if(command_allowed && rearm_request &&
                   (VEHICLE_RESULT_OK == command_result))
                {
                    vehicle_command_t stale_navigation;
                    control_mode = VEHICLE_MODE_AUTOMATIC;
                    safety_manual_latched = false;
                    awaiting_navigation_rearm = true;
                    navigation_motion_active = false;
                    navigation_turn_direction = VEHICLE_MANUAL_STOP;
                    navigation_turn_speed_percent = 0U;
                    navigation_turn_effort = 0U;
                    (void) vehicle_command_mailbox_navigation_take(&stale_navigation);
                    automatic_enter_tick = xTaskGetTickCount();
                    vehicle_yolo_guard_reset(&yolo_guard);
                    navigation_ipc_auto_rearm_request();
                    g_printf("[VEHICLE][SAFE] new NRF AUTO accepted; "
                             "waiting for fresh M85 navigation decision.\r\n");
                }
                else if(command_allowed &&
                   (mode_command || auto_start_command) &&
                   (VEHICLE_RESULT_OK == command_result))
                {
                    /* START_AUTOMATIC本身也会切换底层模式，必须同步软件仲裁状态。 */
                    control_mode = auto_start_command ?
                        VEHICLE_MODE_AUTOMATIC : command.mode;
                    navigation_motion_active = false;
                    navigation_turn_direction = VEHICLE_MANUAL_STOP;
                    navigation_turn_speed_percent = 0U;
                    navigation_turn_effort = 0U;
                    if(VEHICLE_MODE_AUTOMATIC == control_mode)
                    {
                        vehicle_command_t stale_navigation;
                        (void) vehicle_command_mailbox_navigation_take(&stale_navigation);
                        automatic_enter_tick = xTaskGetTickCount();
                        vehicle_yolo_guard_reset(&yolo_guard);
                    }
                    g_printf("[VEHICLE] control mode=%s.\r\n",
                             (VEHICLE_MODE_AUTOMATIC == control_mode) ?
                             "AUTOMATIC" : "MANUAL");
                }
            }
#if VEHICLE_WHEEL_STRAIGHT_TEST_ENABLE
            if (command_requests_stop(&command))
            {
                wheel_test_state = VEHICLE_WHEEL_TEST_COMPLETE;
                g_printf("[VEHICLE][TEST] aborted by stop command\r\n");
            }
#endif
        }

        vehicle_command_t navigation_command;
        if(vehicle_command_mailbox_navigation_take(&navigation_command))
        {
            bool const latch_request =
                VEHICLE_COMMAND_NAVIGATION_MANUAL_LATCH == navigation_command.kind;
            bool const rearm_acknowledgement =
                VEHICLE_COMMAND_NAVIGATION_REARMED_STOP == navigation_command.kind;
            if(latch_request && safety_manual_latched)
            {
                safety_latch_navigation_sequence = (uint8_t) navigation_command.sequence;
            }

            bool const fresh_for_automatic =
                ((int32_t) (navigation_command.received_tick - automatic_enter_tick) >= 0);
            bool const fresh_after_rearm = !awaiting_navigation_rearm ||
                (rearm_acknowledgement && navigation_sequence_is_newer(
                    navigation_command.sequence,
                    safety_latch_navigation_sequence));

            if(latch_request && !safety_manual_latched &&
               !awaiting_navigation_rearm &&
               (VEHICLE_MODE_AUTOMATIC == control_mode) && fresh_for_automatic)
            {
                vehicle_result_t const latch_result = execute_command(&navigation_command);
                if(VEHICLE_RESULT_OK == latch_result)
                {
                    control_mode = VEHICLE_MODE_MANUAL;
                    safety_manual_latched = true;
                    awaiting_navigation_rearm = false;
                    safety_latch_tick = xTaskGetTickCount();
                    safety_latch_navigation_sequence =
                        (uint8_t) navigation_command.sequence;
                    navigation_motion_active = false;
                    navigation_turn_direction = VEHICLE_MANUAL_STOP;
                    g_printf("[VEHICLE][SAFE] M85 escape attempts exhausted; "
                             "manual latch entered.\r\n");
                }
            }
            else if((VEHICLE_MODE_AUTOMATIC == control_mode) &&
                    !safety_manual_latched && fresh_for_automatic &&
                    fresh_after_rearm)
            {
                awaiting_navigation_rearm = false;
                vehicle_result_t const navigation_result =
                    execute_command(&navigation_command);
                if(VEHICLE_RESULT_OK == navigation_result)
                {
                    last_navigation_tick = xTaskGetTickCount();
                    last_navigation_sequence = (uint8_t) navigation_command.sequence;
                    navigation_motion_active =
                        !rearm_acknowledgement &&
                        !command_requests_stop(&navigation_command);
                    if((VEHICLE_MANUAL_TURN_LEFT == navigation_command.manual_action) ||
                       (VEHICLE_MANUAL_TURN_RIGHT == navigation_command.manual_action))
                    {
                        if(navigation_turn_direction != navigation_command.manual_action)
                        {
                            navigation_turn_effort = 0U;
                        }
                        navigation_turn_direction = navigation_command.manual_action;
                        navigation_turn_speed_percent = navigation_command.speed_percent;
                    }
                    else if(VEHICLE_MANUAL_FORWARD == navigation_command.manual_action)
                    {
                        navigation_turn_direction = VEHICLE_MANUAL_STOP;
                        navigation_turn_speed_percent = 0U;
                        navigation_turn_effort = 0U;
                    }
                }
                else if(navigation_result != last_command_error)
                {
                    g_printf("[VEHICLE][NAV ERR] result=%u action=%u speed=%u.\r\n",
                             (unsigned int) navigation_result,
                             (unsigned int) navigation_command.manual_action,
                             (unsigned int) navigation_command.speed_percent);
                    last_command_error = navigation_result;
                }
            }
        }

#if VEHICLE_WHEEL_STRAIGHT_TEST_ENABLE
        if (VEHICLE_WHEEL_TEST_WAITING == wheel_test_state)
        {
            if ((xTaskGetTickCount() - wheel_test_tick) >=
                pdMS_TO_TICKS(VEHICLE_WHEEL_TEST_START_DELAY_MS))
            {
                if (VEHICLE_RESULT_OK ==
                    vehicle_service_manual_command(VEHICLE_MANUAL_FORWARD,
                                                   VEHICLE_WHEEL_TEST_SPEED_PERCENT))
                {
                    wheel_test_state = VEHICLE_WHEEL_TEST_RUNNING;
                    wheel_test_tick = xTaskGetTickCount();
                    g_printf("[VEHICLE][TEST] forward, speed=%u%%\r\n",
                             VEHICLE_WHEEL_TEST_SPEED_PERCENT);
                }
                else
                {
                    /* 吸附尚未建立时稍后重试，不绕过车辆服务的安全检查。 */
                    wheel_test_tick = xTaskGetTickCount() -
                                      pdMS_TO_TICKS(VEHICLE_WHEEL_TEST_START_DELAY_MS - 100U);
                }
            }
        }
        else if ((VEHICLE_WHEEL_TEST_RUNNING == wheel_test_state) &&
                 ((xTaskGetTickCount() - wheel_test_tick) >=
                  pdMS_TO_TICKS(VEHICLE_WHEEL_TEST_RUN_TIME_MS)))
        {
            (void) vehicle_service_manual_command(VEHICLE_MANUAL_STOP, 0U);
            wheel_test_state = VEHICLE_WHEEL_TEST_COMPLETE;
            navigation_motion_active = false;
            g_printf("[VEHICLE][TEST] complete, wheels stopped\r\n");
        }
#endif

        /* 遥控器当前只发送按下/释放边沿，NRF失联保护需周期重发后才能恢复。 */
        if (navigation_motion_active &&
            ((xTaskGetTickCount() - last_navigation_tick) >
             pdMS_TO_TICKS(VEHICLE_NAV_COMMAND_TIMEOUT_MS)))
        {
            (void) vehicle_service_navigation_command(VEHICLE_MANUAL_STOP, 0U);
            navigation_motion_active = false;
            g_printf("[VEHICLE][SAFE] navigation timeout; wheels stopped.\r\n");
        }

        vehicle_result_t const step_result =
            vehicle_service_step((float) VEHICLE_CONTROL_PERIOD_MS / 1000.0F);
        if (VEHICLE_RESULT_OK != step_result)
        {
            vehicle_status_t status;
            vehicle_service_status_get(&status);
            if (VEHICLE_RESULT_SENSOR_ERROR == step_result)
            {
                log_i2c_error_if_changed();
            }
            g_printf("[VEHICLE][STEP ERR] result=%u last=%u suction_ready=%u "
                     "heading=%u duty_milli(L/R/F)=%d/%d/%d; emergency stop.\r\n",
                     (unsigned int) step_result,
                     (unsigned int) status.last_error,
                     (unsigned int) status.suction_ready,
                     (unsigned int) status.heading_enabled,
                     (int) (status.left_duty * 1000.0F),
                     (int) (status.right_duty * 1000.0F),
                     (int) (status.suction_duty * 1000.0F));
            vehicle_service_emergency_stop();
        }
        vTaskDelayUntil(&wake_tick, pdMS_TO_TICKS(VEHICLE_CONTROL_PERIOD_MS));
    }
}
