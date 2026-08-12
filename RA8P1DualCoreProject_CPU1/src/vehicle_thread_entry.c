#include <vehicle_thread.h>
#include "Vehicle/adapters/rtos/vehicle_command_mailbox.h"
#include "Vehicle/application/vehicle_service.h"
#include "Vehicle/platform/fsp_vehicle_factory.h"
#include "SEGGER_RTT/bsp_print.h"

#define VEHICLE_CONTROL_PERIOD_MS        (10U)
#define VEHICLE_COMMAND_TIMEOUT_MS       (200U)

static void execute_command(vehicle_command_t const * command)
{
    switch (command->kind)
    {
        case VEHICLE_COMMAND_MANUAL:
            (void) vehicle_service_manual_command(command->manual_action,
                                                  command->speed_percent);
            break;
        case VEHICLE_COMMAND_SET_SUCTION:
            (void) vehicle_service_suction_set(command->suction_enable,
                                               command->suction_percent);
            break;
        case VEHICLE_COMMAND_START_AUTOMATIC:
            (void) vehicle_service_automatic_start();
            break;
        case VEHICLE_COMMAND_SET_MODE:
            (void) vehicle_service_mode_set(command->mode);
            break;
        case VEHICLE_COMMAND_EMERGENCY_STOP:
            vehicle_service_emergency_stop();
            break;
        default:
            break;
    }
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
    TickType_t wake_tick;
    TickType_t last_command_tick;
    bool remote_command_seen = false;

    FSP_PARAMETER_NOT_USED(pvParameters);

    if ((!vehicle_command_mailbox_init()) ||
        (!fsp_vehicle_dependencies_create(&dependencies)) ||
        (VEHICLE_RESULT_OK != vehicle_service_init(&dependencies)))
    {
        vehicle_service_emergency_stop();
        g_printf("[VEHICLE][FATAL] initialization failed\r\n");
        vTaskSuspend(NULL);
    }

    wake_tick = xTaskGetTickCount();
    last_command_tick = wake_tick;
    for (;;)
    {
        if (vehicle_command_mailbox_take(&command))
        {
            execute_command(&command);
            last_command_tick = xTaskGetTickCount();
            remote_command_seen = VEHICLE_COMMAND_SOURCE_NRF == command.source;
        }

        /* 手持遥控曾接管后，超过 200 ms 未收到新命令则自主停车。 */
        if (remote_command_seen &&
            ((xTaskGetTickCount() - last_command_tick) >
             pdMS_TO_TICKS(VEHICLE_COMMAND_TIMEOUT_MS)))
        {
            (void) vehicle_service_manual_command(VEHICLE_MANUAL_STOP, 0U);
            remote_command_seen = false;
        }

        if (VEHICLE_RESULT_OK !=
            vehicle_service_step((float) VEHICLE_CONTROL_PERIOD_MS / 1000.0F))
        {
            vehicle_service_emergency_stop();
        }
        vTaskDelayUntil(&wake_tick, pdMS_TO_TICKS(VEHICLE_CONTROL_PERIOD_MS));
    }
}
