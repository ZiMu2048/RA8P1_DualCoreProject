#include "ipc_thread.h"
#include "IPC/navigation_ipc_protocol.h"
#include "IPC/shared_jpeg_cpu0.h"
#include "IPC/yolo_safety_ipc_protocol.h"
#include "IPC/yolo_safety_ipc_runtime.h"
#include "Navigation/navigation_runtime.h"
#include "SEGGER_RTT/bsp_print.h"

extern TaskHandle_t ipc_thread;

#define YOLO_SAFETY_CPU0_QUEUE_LENGTH    (8U)
#define YOLO_SAFETY_IPC_RETRY_MS         (5U)

typedef struct st_yolo_safety_cpu0_item
{
    uint32_t frame_sequence;
    bool detected;
} yolo_safety_cpu0_item_t;

static yolo_safety_cpu0_item_t
    g_yolo_safety_queue[YOLO_SAFETY_CPU0_QUEUE_LENGTH];
static uint32_t g_yolo_safety_queue_read;
static uint32_t g_yolo_safety_queue_write;
static uint32_t g_yolo_safety_queue_count;
static bool g_yolo_safety_send_pending;
static yolo_safety_cpu0_item_t g_yolo_safety_send_item;

bool yolo_safety_ipc_publish(uint32_t frame_sequence, bool detected)
{
    taskENTER_CRITICAL();
    if(g_yolo_safety_queue_count >= YOLO_SAFETY_CPU0_QUEUE_LENGTH)
    {
        /* 发送端拥塞时丢弃最旧结果，保留与当前路况最接近的推理。 */
        g_yolo_safety_queue_read =
            (g_yolo_safety_queue_read + 1U) % YOLO_SAFETY_CPU0_QUEUE_LENGTH;
        g_yolo_safety_queue_count--;
    }

    g_yolo_safety_queue[g_yolo_safety_queue_write].frame_sequence =
        frame_sequence;
    g_yolo_safety_queue[g_yolo_safety_queue_write].detected = detected;
    g_yolo_safety_queue_write =
        (g_yolo_safety_queue_write + 1U) % YOLO_SAFETY_CPU0_QUEUE_LENGTH;
    g_yolo_safety_queue_count++;
    taskEXIT_CRITICAL();

    xTaskNotifyGive(ipc_thread);
    return true;
}

static bool yolo_safety_ipc_send_service(void)
{
    bool work_pending;

    if(!g_yolo_safety_send_pending)
    {
        taskENTER_CRITICAL();
        if(g_yolo_safety_queue_count > 0U)
        {
            g_yolo_safety_send_item =
                g_yolo_safety_queue[g_yolo_safety_queue_read];
            g_yolo_safety_queue_read =
                (g_yolo_safety_queue_read + 1U) % YOLO_SAFETY_CPU0_QUEUE_LENGTH;
            g_yolo_safety_queue_count--;
            g_yolo_safety_send_pending = true;
        }
        taskEXIT_CRITICAL();
    }

    if(g_yolo_safety_send_pending)
    {
        uint32_t const message = yolo_safety_ipc_message_encode(
            (uint8_t) g_yolo_safety_send_item.frame_sequence,
            g_yolo_safety_send_item.detected);
        if(FSP_SUCCESS == g_ipc0.p_api->messageSend(g_ipc0.p_ctrl, message))
        {
            g_yolo_safety_send_pending = false;
        }
    }

    taskENTER_CRITICAL();
    work_pending = g_yolo_safety_send_pending ||
                   (g_yolo_safety_queue_count > 0U);
    taskEXIT_CRITICAL();
    return work_pending;
}

/*
 *[@name] g_ipc0_callback
 *[@type] IPC interrupt callback
 *[@usage] 保存CPU1的共享JPEG回执并使用任务通知唤醒CPU0 IPC Thread
 *[@argument] p_args FSP提供的IPC通道、消息、事件和用户上下文
 *[@return] none
 */
void g_ipc0_callback(ipc_callback_args_t * p_args)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    if((NULL != p_args) &&
       (0U != ((uint32_t) p_args->event & (uint32_t) IPC_EVENT_MESSAGE_RECEIVED)))
    {
        nav_ipc_control_t control;
        if(nav_ipc_control_message_decode(p_args->message, &control) &&
           (NAV_IPC_CONTROL_AUTO_REARM == control))
        {
            navigation_auto_rearm_from_isr();
        }
        else
        {
            shared_jpeg_cpu0_on_ipc_message_isr(p_args->message);
            vTaskNotifyGiveFromISR(ipc_thread, &higher_priority_task_woken);
        }
        portYIELD_FROM_ISR(higher_priority_task_woken);
    }
}

/*
 *[@name] ipc_thread_entry
 *[@type] thread entry function
 *[@usage] 初始化CPU0共享JPEG生产端，阻塞等待CPU1回执并释放共享载荷所有权
 *[@argument] pvParameters FSP传入的线程参数，当前未使用
 *[@return] none
 */
void ipc_thread_entry(void * pvParameters)
{
    fsp_err_t err;

    FSP_PARAMETER_NOT_USED(pvParameters);

    err = shared_jpeg_cpu0_init();
    if(FSP_SUCCESS != err)
    {
        g_printf("[SHM0][FATAL] Init failed: %u.\r\n", (unsigned int) err);
        vTaskSuspend(NULL);
    }

    g_printf("[SHM0] Ready: base=0x%08X capacity=%u.\r\n",
             (unsigned int) SHARED_JPEG_BASE_ADDRESS,
             (unsigned int) SHARED_JPEG_PAYLOAD_CAPACITY);

    for(;;)
    {
        shared_jpeg_completion_t completion;
        shared_jpeg_cpu0_result_t const result =
            shared_jpeg_cpu0_poll(&completion);

        if(SHARED_JPEG_CPU0_TIMEOUT == result)
        {
            g_printf("[SHM0][ERR] CPU1 JPEG acknowledgement timeout frame=%u.\r\n",
                     (unsigned int) completion.frame_sequence);
        }
        else if(SHARED_JPEG_CPU0_NOTIFY_PENDING == result)
        {
            g_printf("[SHM0][WARN] Doorbell retry pending.\r\n");
        }
        else if(SHARED_JPEG_CPU0_SUCCESS != result)
        {
            g_printf("[SHM0][ERR] Poll failed: %u.\r\n",
                     (unsigned int) result);
        }
        else if(completion.completed)
        {
            if(completion.succeeded)
            {
            }
            else
            {
                g_printf("[SHM0][ERR] CPU1 rejected frame=%u error=%u.\r\n",
                         (unsigned int) completion.frame_sequence,
                         (unsigned int) completion.error_code);
            }
        }

        bool const yolo_work_pending = yolo_safety_ipc_send_service();
        (void) ulTaskNotifyTake(
            pdTRUE,
            pdMS_TO_TICKS(yolo_work_pending ? YOLO_SAFETY_IPC_RETRY_MS : 100U));
    }
}
