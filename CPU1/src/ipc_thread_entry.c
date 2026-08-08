#include <ipc_thread.h>
#include <stdbool.h>
#include "SEGGER_RTT/bsp_print.h"
#include "shared_mem_protocol.h"

/*
 * Core: CPU1 / Cortex-M33
 * Send:    g_ipc0, channel 0, to CPU0
 * Receive: g_ipc1, channel 1, from CPU0
 */

#define IPC_MSG_TYPE_MASK       (0xFF000000UL)
#define IPC_MSG_COUNTER_MASK    (0x00FFFFFFUL)
#define IPC_MSG_PING            (0xA1000000UL)
#define IPC_MSG_ACK             (0xA2000000UL)

#define IPC_STARTUP_DELAY_MS     (200UL)
#define IPC_ACK_TIMEOUT_MS       (500UL)
#define IPC_TEST_PERIOD_MS       (1000UL)

static TaskHandle_t s_cpu1_ipc_task = NULL;

/* 在调试器 Expressions 窗口中观察这些变量 */
volatile uint32_t g_cpu1_ipc_rx_count       = 0;
volatile uint32_t g_cpu1_ipc_tx_count       = 0;
volatile uint32_t g_cpu1_ipc_timeout_count  = 0;
volatile uint32_t g_cpu1_ipc_protocol_error = 0;
volatile uint32_t g_cpu1_ipc_last_rx        = 0;
volatile uint32_t g_cpu1_ipc_last_tx        = 0;
volatile uint32_t g_cpu1_ipc_last_event     = 0;
volatile fsp_err_t g_cpu1_ipc_last_error    = FSP_SUCCESS;

volatile TaskHandle_t g_stack_overflow_task = NULL;
volatile char const * g_stack_overflow_task_name = NULL;
volatile uint32_t g_stack_overflow_count = 0;

/* 跨核共享内存测试诊断变量 */
volatile uint32_t g_cpu1_shared_test_fail_index = 0;
volatile uint32_t g_cpu1_shared_test_expected   = 0;
volatile uint32_t g_cpu1_shared_test_actual     = 0;
volatile uint32_t g_cpu1_shared_test_passed     = 0;

static void cpu1_ipc_error_wait(fsp_err_t err)
{
    g_cpu1_ipc_last_error = err;

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/*
 * IPC中断回调。
 * 这里只保存32位ACK并通知IPC任务。
 */
void cpu1_ipc_callback(ipc_callback_args_t * p_args)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    if (NULL == p_args)
    {
        return;
    }

    g_cpu1_ipc_last_event = (uint32_t) p_args->event;

    if (IPC_EVENT_MESSAGE_RECEIVED == p_args->event)
    {
        g_cpu1_ipc_last_rx = p_args->message;
        g_cpu1_ipc_rx_count++;

        if (NULL != s_cpu1_ipc_task)
        {
            (void) xTaskNotifyFromISR(s_cpu1_ipc_task,
                                     p_args->message,
                                     eSetValueWithOverwrite,
                                     &higher_priority_task_woken);

            portYIELD_FROM_ISR(higher_priority_task_woken);
        }
    }
}

/* 测试辅助函数：计算共享载荷的CRC32。 */
static uint32_t cpu1_shared_mem_test_crc32(volatile uint8_t const * p_data,
                                          uint32_t length)
{
    uint32_t crc = 0xFFFFFFFFUL;

    for (uint32_t i = 0; i < length; i++)
    {
        crc ^= p_data[i];

        for (uint32_t bit = 0; bit < 8UL; bit++)
        {
            crc = (0UL != (crc & 1UL)) ?
                  ((crc >> 1) ^ 0xEDB88320UL) :
                  (crc >> 1);
        }
    }

    return crc ^ 0xFFFFFFFFUL;
}

/*
 * 测试函数：CPU1校验CPU0发布的共享协议消息并写回响应。
 * 共享对象由CPU0链接到0x68800000，CPU1仅通过固定地址访问。
 */
static bool cpu1_shared_mem_round_trip_test(void)
{
    volatile shared_mem_control_block_t * const p_shared =
        (volatile shared_mem_control_block_t *) 0x68800000UL;

    g_cpu1_shared_test_fail_index = 0;
    g_cpu1_shared_test_expected   = 0;
    g_cpu1_shared_test_actual     = 0;
    g_cpu1_shared_test_passed     = 0;

    /* 保证CPU1在读取前完成对共享内存的观察顺序约束 */
    __DMB();

    /* 校验READY状态及CPU0发布的全部协议字段 */
    if ((p_shared->magic != SHARED_MEM_MAGIC) ||
        (p_shared->version != SHARED_MEM_VERSION) ||
        (p_shared->state != SHARED_MEM_STATE_READY) ||
        (p_shared->owner != SHARED_MEM_OWNER_CPU1) ||
        (p_shared->sequence != 1UL) ||
        (p_shared->data_length != SHARED_MEM_PAYLOAD_SIZE) ||
        (p_shared->error_code != 0UL))
    {
        g_cpu1_shared_test_expected = SHARED_MEM_STATE_READY;
        g_cpu1_shared_test_actual   = p_shared->state;

        return false;
    }

    const uint32_t actual_crc =
        cpu1_shared_mem_test_crc32(p_shared->payload,
                                   p_shared->data_length);

    if (actual_crc != p_shared->crc32)
    {
        g_cpu1_shared_test_expected = p_shared->crc32;
        g_cpu1_shared_test_actual   = actual_crc;

        return false;
    }

    /* 校验CPU0发布的初始载荷 */
    for (uint32_t i = 0; i < SHARED_MEM_PAYLOAD_SIZE; i++)
    {
        const uint8_t expected =
            (uint8_t) ((i * 37UL) ^ 0xA5UL);

        const uint8_t actual = p_shared->payload[i];

        if (actual != expected)
        {
            g_cpu1_shared_test_fail_index = i;
            g_cpu1_shared_test_expected   = expected;
            g_cpu1_shared_test_actual     = actual;

            return false;
        }
    }

    /* CPU1取得所有权并进入PROCESSING状态 */
    p_shared->state = SHARED_MEM_STATE_PROCESSING;
    __DMB();

    /* 写入响应载荷和元数据 */
    for (uint32_t i = 0; i < SHARED_MEM_PAYLOAD_SIZE; i++)
    {
        p_shared->payload[i] =
            (uint8_t) ((i * 53UL) ^ 0x5AUL);
    }

    p_shared->data_length = SHARED_MEM_PAYLOAD_SIZE;
    p_shared->crc32 =
        cpu1_shared_mem_test_crc32(p_shared->payload,
                                   p_shared->data_length);
    p_shared->error_code = 0UL;
    p_shared->owner      = SHARED_MEM_OWNER_CPU0;

    /* DONE最后发布，随后CPU1才允许发送PING通知CPU0 */
    __DMB();
    p_shared->state = SHARED_MEM_STATE_DONE;
    __DSB();

    g_cpu1_shared_test_passed = 1;

    return true;
}

/*
 * CPU1 IPC线程入口。
 * CPU1每秒发送一个递增PING，并等待CPU0返回相同序号的ACK。
 */
void ipc_thread_entry(void * pvParameters)
{
    fsp_err_t err;
    uint32_t counter = 0;
    uint32_t ping_message;
    uint32_t received_message;

    FSP_PARAMETER_NOT_USED(pvParameters);

    SEGGER_RTT_Init();
    g_printf("[CPU1] RTT initialized\r\n");

    if (cpu1_shared_mem_round_trip_test())
    {
        g_printf("[CPU1] CPU0 SHARED_MEM pattern PASS, response published\r\n");
    }
    else
    {
        g_printf("[CPU1] CPU0 SHARED_MEM pattern FAIL "
                 "INDEX=%u EXP=%02X ACT=%02X\r\n",
                 (unsigned int) g_cpu1_shared_test_fail_index,
                 (unsigned int) g_cpu1_shared_test_expected,
                 (unsigned int) g_cpu1_shared_test_actual);

        while (1)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    s_cpu1_ipc_task = xTaskGetCurrentTaskHandle();

    /*
     * 先打开CPU1接收通道。
     * CPU1通过channel 1接收CPU0回复。
     */
    err = g_ipc1.p_api->open(g_ipc1.p_ctrl, g_ipc1.p_cfg);
    if (FSP_SUCCESS != err)
    {
        cpu1_ipc_error_wait(err);
    }

    /*
     * 再打开CPU1发送通道。
     * CPU1通过channel 0向CPU0发送。
     */
    err = g_ipc0.p_api->open(g_ipc0.p_ctrl, g_ipc0.p_cfg);
    if (FSP_SUCCESS != err)
    {
        cpu1_ipc_error_wait(err);
    }

    /*
     * 给CPU0调度器和IPC接收中断留出启动时间。
     * 即使第一次超时，后续循环仍会继续重试。
     */
    vTaskDelay(pdMS_TO_TICKS(IPC_STARTUP_DELAY_MS));

    while (1)
    {
        counter = (counter + 1UL) & IPC_MSG_COUNTER_MASK;

        if (0UL == counter)
        {
            counter = 1UL;
        }

        ping_message = IPC_MSG_PING | counter;
        received_message = 0;

        err = g_ipc0.p_api->messageSend(g_ipc0.p_ctrl,
                                        ping_message);

        g_cpu1_ipc_last_error = err;

        if (FSP_SUCCESS == err)
        {
            g_cpu1_ipc_last_tx = ping_message;
            g_cpu1_ipc_tx_count++;

            if (pdTRUE == xTaskNotifyWait(0,
                                          0xFFFFFFFFUL,
                                          &received_message,
                                          pdMS_TO_TICKS(IPC_ACK_TIMEOUT_MS)))
            {
                if (((received_message & IPC_MSG_TYPE_MASK) == IPC_MSG_ACK) &&
                    ((received_message & IPC_MSG_COUNTER_MASK) == counter))
                {
                    /* 正确ACK已经由回调计入g_cpu1_ipc_rx_count。 */
                }
                else
                {
                    g_cpu1_ipc_protocol_error++;
                }
            }
            else
            {
                g_cpu1_ipc_timeout_count++;
            }
        }

        g_printf("[CPU1] TX=%08X RX=%08X TX_CNT=%u RX_CNT=%u "
                 "TIMEOUT=%u PROTO_ERR=%u FSP_ERR=%u STACK_OVF=%u\r\n",
                 (unsigned int) g_cpu1_ipc_last_tx,
                 (unsigned int) g_cpu1_ipc_last_rx,
                 (unsigned int) g_cpu1_ipc_tx_count,
                 (unsigned int) g_cpu1_ipc_rx_count,
                 (unsigned int) g_cpu1_ipc_timeout_count,
                 (unsigned int) g_cpu1_ipc_protocol_error,
                 (unsigned int) g_cpu1_ipc_last_error,
                 (unsigned int) g_stack_overflow_count);

        vTaskDelay(pdMS_TO_TICKS(IPC_TEST_PERIOD_MS));
    }
}

/*
 * FreeRTOS stack-overflow hook.
 * 每个内核单独拥有一份，不涉及跨核共享。
 */
void vApplicationStackOverflowHook(TaskHandle_t xTask,
                                   char * pcTaskName)
{
    /*
     * 先保存现场信息，随后关闭当前内核中断并停机。
     * 不在这里打印、延时或调用可能再次使用栈的复杂函数。
     */
    g_stack_overflow_task      = xTask;
    g_stack_overflow_task_name = pcTaskName;
    g_stack_overflow_count++;

    taskDISABLE_INTERRUPTS();

    while (1)
    {
        __NOP();
    }
}
