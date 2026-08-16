#include "ipc_thread.h"
#include "app_runtime.h"
#include "IPC/navigation_ipc_protocol.h"
#include "IPC/navigation_ipc_runtime.h"
#include "IPC/shared_jpeg_cpu1.h"
#include "IPC/yolo_safety_ipc_protocol.h"
#include "IPC/yolo_safety_runtime.h"
#include "Vehicle/adapters/rtos/vehicle_command_mailbox.h"
#include "WifiUpload/wifi_upload_mailbox.h"
#include "Radio/adapters/rtos/video_frame_mailbox.h"
#include "Radio/protocol/video_protocol.h"
#include "SEGGER_RTT/bsp_print.h"

/* 导航速度参数集中在M33输入适配层，后续实车只需调整这些宏。 */
#define NAV_FORWARD_SPEED_PERCENT       (90U)  /* MPU隔离阶段的自动前进PWM百分比。 */
#define NAV_LEFT_TURN_SPEED_PERCENT     (100U) /* MPU隔离阶段的原地左转PWM百分比。 */
#define NAV_RIGHT_TURN_SPEED_PERCENT    (100U) /* MPU隔离阶段的原地右转PWM百分比。 */
#define NAV_IPC_ACTION_RTT_ENABLE       (0U)   /* 置1后打印请求动作和实际下发动作。 */
#define YOLO_SAFETY_CPU1_QUEUE_LENGTH   (8U)

static volatile bool g_navigation_message_pending;
static volatile uint32_t g_navigation_message;
static volatile bool g_navigation_auto_rearm_pending;
static yolo_safety_result_t g_yolo_safety_results[YOLO_SAFETY_CPU1_QUEUE_LENGTH];
static volatile uint32_t g_yolo_safety_result_read;
static volatile uint32_t g_yolo_safety_result_write;
static volatile uint32_t g_yolo_safety_result_count;
extern TaskHandle_t ipc_thread;

bool yolo_safety_result_take(yolo_safety_result_t * p_result)
{
    bool available = false;

    if(NULL == p_result)
    {
        return false;
    }

    taskENTER_CRITICAL();
    if(g_yolo_safety_result_count > 0U)
    {
        *p_result = g_yolo_safety_results[g_yolo_safety_result_read];
        g_yolo_safety_result_read =
            (g_yolo_safety_result_read + 1U) % YOLO_SAFETY_CPU1_QUEUE_LENGTH;
        g_yolo_safety_result_count--;
        available = true;
    }
    taskEXIT_CRITICAL();
    return available;
}

static void yolo_safety_result_publish_from_isr(
    yolo_safety_ipc_result_t const * p_ipc_result)
{
    if(g_yolo_safety_result_count >= YOLO_SAFETY_CPU1_QUEUE_LENGTH)
    {
        g_yolo_safety_result_read =
            (g_yolo_safety_result_read + 1U) % YOLO_SAFETY_CPU1_QUEUE_LENGTH;
        g_yolo_safety_result_count--;
    }

    g_yolo_safety_results[g_yolo_safety_result_write].frame_sequence =
        p_ipc_result->frame_sequence;
    g_yolo_safety_results[g_yolo_safety_result_write].detected =
        p_ipc_result->detected;
    g_yolo_safety_result_write =
        (g_yolo_safety_result_write + 1U) % YOLO_SAFETY_CPU1_QUEUE_LENGTH;
    __DMB();
    g_yolo_safety_result_count++;
}

#if NAV_IPC_ACTION_RTT_ENABLE
static const char * navigation_action_name(nav_ipc_action_t action)
{
    switch(action)
    {
        case NAV_IPC_ACTION_FORWARD:   return "FORWARD";
        case NAV_IPC_ACTION_TURN_LEFT: return "TURN_LEFT";
        case NAV_IPC_ACTION_TURN_RIGHT: return "TURN_RIGHT";
        case NAV_IPC_ACTION_MANUAL_LATCH: return "MANUAL_LATCH";
        case NAV_IPC_ACTION_AUTO_REARMED_STOP: return "AUTO_REARMED_STOP";
        case NAV_IPC_ACTION_STOP:
        default:                       return "STOP";
    }
}
#endif

void navigation_ipc_auto_rearm_request(void)
{
    taskENTER_CRITICAL();
    g_navigation_auto_rearm_pending = true;
    taskEXIT_CRITICAL();
    xTaskNotifyGive(ipc_thread);
}

static void navigation_auto_rearm_send_service(void)
{
    bool pending;

    taskENTER_CRITICAL();
    pending = g_navigation_auto_rearm_pending;
    g_navigation_auto_rearm_pending = false;
    taskEXIT_CRITICAL();

    if(pending &&
       (FSP_SUCCESS != g_ipc1.p_api->messageSend(
            g_ipc1.p_ctrl,
            nav_ipc_control_message_encode(NAV_IPC_CONTROL_AUTO_REARM))))
    {
        taskENTER_CRITICAL();
        g_navigation_auto_rearm_pending = true;
        taskEXIT_CRITICAL();
    }
}

static bool navigation_message_take(uint32_t * p_message)
{
    bool pending;

    taskENTER_CRITICAL();
    pending = g_navigation_message_pending;
    if(pending)
    {
        *p_message = g_navigation_message;
        g_navigation_message_pending = false;
    }
    taskEXIT_CRITICAL();
    return pending;
}

static void navigation_command_dispatch(uint32_t message)
{
#if NAV_IPC_ACTION_RTT_ENABLE
    static nav_ipc_action_t last_logged_request = NAV_IPC_ACTION_COUNT;
    static nav_ipc_action_t last_logged_applied = NAV_IPC_ACTION_COUNT;
#endif
    nav_ipc_action_t requested_action;
    uint8_t sequence;

    if(!nav_ipc_message_decode(message, &requested_action, &sequence))
    {
        return;
    }

    vehicle_command_t command =
    {
        .source = VEHICLE_COMMAND_SOURCE_IPC,
        .kind = VEHICLE_COMMAND_MANUAL,
        .sequence = sequence,
        .received_tick = xTaskGetTickCount(),
        .manual_action = VEHICLE_MANUAL_STOP,
        .speed_percent = 0U,
    };

    switch(requested_action)
    {
        case NAV_IPC_ACTION_FORWARD:
            command.manual_action = VEHICLE_MANUAL_FORWARD;
            command.speed_percent = NAV_FORWARD_SPEED_PERCENT;
            break;
        case NAV_IPC_ACTION_TURN_LEFT:
            command.manual_action = VEHICLE_MANUAL_TURN_LEFT;
            command.speed_percent = NAV_LEFT_TURN_SPEED_PERCENT;
            break;
        case NAV_IPC_ACTION_TURN_RIGHT:
            command.manual_action = VEHICLE_MANUAL_TURN_RIGHT;
            command.speed_percent = NAV_RIGHT_TURN_SPEED_PERCENT;
            break;
        case NAV_IPC_ACTION_MANUAL_LATCH:
            command.kind = VEHICLE_COMMAND_NAVIGATION_MANUAL_LATCH;
            break;
        case NAV_IPC_ACTION_AUTO_REARMED_STOP:
            command.kind = VEHICLE_COMMAND_NAVIGATION_REARMED_STOP;
            break;
        case NAV_IPC_ACTION_STOP:
        default:
            break;
    }

    if(!vehicle_command_mailbox_submit(&command))
    {
        g_printf("[NAV IPC][ERR] Vehicle mailbox unavailable.\r\n");
        return;
    }

#if NAV_IPC_ACTION_RTT_ENABLE
    if((requested_action != last_logged_request) ||
       (requested_action != last_logged_applied))
    {
        g_printf("[NAV IPC] seq=%u request=%s applied=%s speed=%u%%.\r\n",
                 (unsigned int) sequence,
                 navigation_action_name(requested_action),
                 navigation_action_name(requested_action),
                 (unsigned int) command.speed_percent);
        last_logged_request = requested_action;
        last_logged_applied = requested_action;
    }
#endif
}

/*
 *[@name] g_ipc1_callback
 *[@type] IPC interrupt callback
 *[@usage] 保存CPU0的DATA_READY门铃并使用任务通知唤醒CPU1 IPC Thread
 *[@argument] p_args FSP提供的IPC通道、消息、事件和用户上下文
 *[@return] none
 */
void g_ipc1_callback(ipc_callback_args_t * p_args)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    if((NULL != p_args) &&
       (0U != ((uint32_t) p_args->event & (uint32_t) IPC_EVENT_MESSAGE_RECEIVED)))
    {
        nav_ipc_action_t action;
        uint8_t sequence;
        yolo_safety_ipc_result_t yolo_result;
        if(nav_ipc_message_decode(p_args->message, &action, &sequence))
        {
            FSP_PARAMETER_NOT_USED(action);
            FSP_PARAMETER_NOT_USED(sequence);
            g_navigation_message = p_args->message;
            __DMB();
            g_navigation_message_pending = true;
        }
        else if(yolo_safety_ipc_message_decode(p_args->message, &yolo_result))
        {
            yolo_safety_result_publish_from_isr(&yolo_result);
        }
        else
        {
            shared_jpeg_cpu1_on_ipc_message_isr(p_args->message);
        }
        vTaskNotifyGiveFromISR(ipc_thread, &higher_priority_task_woken);
        portYIELD_FROM_ISR(higher_priority_task_woken);
    }
}

/*
 *[@name] ipc_thread_entry
 *[@type] thread entry function
 *[@usage] 初始化CPU1共享JPEG消费者，校验JPEG边界与CRC并通过IPC返回处理结果
 *[@argument] pvParameters FSP传入的线程参数，当前未使用
 *[@return] none
 */
void ipc_thread_entry(void * pvParameters)
{
    fsp_err_t err;
    uint32_t video_sequence_in_flight = 0U;

    FSP_PARAMETER_NOT_USED(pvParameters);

    if(!app_runtime_init())
    {
        g_printf("[SYSTEM][FATAL] IPC runtime initialization failed.\r\n");
        vTaskSuspend(NULL);
    }

    if(!wifi_upload_mailbox_init())
    {
        g_printf("[SHM1][FATAL] Wi-Fi upload queue init failed.\r\n");
        vTaskSuspend(NULL);
    }

    err = shared_jpeg_cpu1_init();
    if(FSP_SUCCESS != err)
    {
        g_printf("[SHM1][FATAL] Init failed: %u.\r\n", (unsigned int) err);
        vTaskSuspend(NULL);
    }

    g_printf("[SHM1] CPU1 ready: base=0x%08X capacity=%u.\r\n",
             (unsigned int) SHARED_JPEG_BASE_ADDRESS,
             (unsigned int) SHARED_JPEG_PAYLOAD_CAPACITY);

    /* IPC callback and shared-memory receiver are ready before business dispatch opens. */
    app_runtime_wait_for_start();

    for(;;)
    {
        uint32_t navigation_message;
        if(navigation_message_take(&navigation_message))
        {
            navigation_command_dispatch(navigation_message);
        }
        navigation_auto_rearm_send_service();

        shared_jpeg_cpu1_report_t report;
        shared_jpeg_cpu1_result_t const result =
            shared_jpeg_cpu1_process(&report);

        if(report.upload_ready)
        {
            wifi_upload_job_t const job =
            {
                .p_jpeg_data = report.p_payload,
                .jpeg_length = report.payload_length,
                .frame_sequence = report.frame_sequence,
                .jpeg_crc32 = report.actual_crc32,
                .width = 240U,
                .height = 240U,
                .confidence_milli = report.confidence_milli
            };

            if(!wifi_upload_mailbox_submit(&job))
            {
                shared_jpeg_cpu1_result_t const completion_result =
                    shared_jpeg_cpu1_complete_upload(
                        report.frame_sequence,
                        false,
                        SHARED_JPEG_ERROR_UPLOAD_QUEUE);

                g_printf("[SHM1][ERR] Wi-Fi queue busy frame=%u.\r\n",
                         (unsigned int) report.frame_sequence);

                if(SHARED_JPEG_CPU1_IPC_ERROR == completion_result)
                {
                    g_printf("[SHM1][WARN] Upload error acknowledgement retry pending.\r\n");
                }
            }
        }
        else if(report.completed)
        {
            if(!report.succeeded)
            {
                g_printf("[SHM1][ERR] JPEG rejected frame=%u error=%u expected=0x%08X actual=0x%08X.\r\n",
                         (unsigned int) report.frame_sequence,
                         (unsigned int) report.error_code,
                         (unsigned int) report.expected_crc32,
                         (unsigned int) report.actual_crc32);
            }

            if(SHARED_JPEG_CPU1_IPC_ERROR == result)
            {
                g_printf("[SHM1][WARN] Result acknowledgement retry pending.\r\n");
            }
        }
        else if((SHARED_JPEG_CPU1_SUCCESS != result) &&
                (SHARED_JPEG_CPU1_NO_DATA != result))
        {
            g_printf("[SHM1][ERR] Process failed: %u.\r\n",
                     (unsigned int) result);
        }

        uint16_t completed_frame_id = 0U;
        bool video_send_succeeded = false;
        if(VideoFrameMailbox_CompletionTake(&completed_frame_id,
                                             &video_send_succeeded) &&
           ((uint16_t) video_sequence_in_flight == completed_frame_id))
        {
            (void) shared_video_cpu1_complete(video_sequence_in_flight,
                                              video_send_succeeded);
            video_sequence_in_flight = 0U;
        }

        if(0U == video_sequence_in_flight)
        {
            shared_video_cpu1_report_t video_report;
            shared_jpeg_cpu1_result_t const video_result =
                shared_video_cpu1_process(&video_report);
            if(video_report.frame_ready)
            {
                video_frame_t const frame =
                {
                    .p_jpeg = video_report.p_payload,
                    .jpeg_size = video_report.payload_length,
                    .crc32 = video_report.payload_crc32,
                    .frame_id = (uint16_t) video_report.frame_sequence,
                    .source_width = video_report.width,
                    .source_height = video_report.height
                };
                if(VideoFrameMailbox_Publish(&frame))
                {
                    video_sequence_in_flight = video_report.frame_sequence;
                }
                else
                {
                    (void) shared_video_cpu1_complete(video_report.frame_sequence,
                                                      false);
                }
            }
            else if((SHARED_JPEG_CPU1_SUCCESS != video_result) &&
                    (SHARED_JPEG_CPU1_NO_DATA != video_result))
            {
                g_printf("[VIDEO IPC][ERR] process=%u.\r\n",
                         (unsigned int) video_result);
            }
        }

        (void) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5U));
    }
}
