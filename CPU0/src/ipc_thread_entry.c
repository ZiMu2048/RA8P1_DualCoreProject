#include <ipc_thread.h>
#include <stdbool.h>
#include "SEGGER_RTT/bsp_print.h"
#include "shared_mem_protocol.h"
#include "camera_i2c_test.h"
/*
 * Core: CPU0 / Cortex-M85
 * Receive: g_ipc0, channel 0, from CPU1
 * Send:    g_ipc1, channel 1, to CPU1
 */

#define IPC_MSG_TYPE_MASK       (0xFF000000UL)
#define IPC_MSG_COUNTER_MASK    (0x00FFFFFFUL)
#define IPC_MSG_PING            (0xA1000000UL)
#define IPC_MSG_ACK             (0xA2000000UL)

static TaskHandle_t s_cpu0_ipc_task = NULL;
static bool s_cpu0_shared_peer_checked = false;

/* 在调试器 Expressions 窗口中观察这些变量 */
volatile uint32_t g_cpu0_ipc_rx_count       = 0;
volatile uint32_t g_cpu0_ipc_tx_count       = 0;
volatile uint32_t g_cpu0_ipc_protocol_error = 0;
volatile uint32_t g_cpu0_ipc_last_rx        = 0;
volatile uint32_t g_cpu0_ipc_last_tx        = 0;
volatile uint32_t g_cpu0_ipc_last_event     = 0;
volatile fsp_err_t g_cpu0_ipc_last_error    = FSP_SUCCESS;

volatile TaskHandle_t g_stack_overflow_task = NULL;
volatile char const * g_stack_overflow_task_name = NULL;
volatile uint32_t g_stack_overflow_count = 0;

volatile uint32_t g_sdram_test_failed_address = 0;
volatile uint32_t g_sdram_test_expected       = 0;
volatile uint32_t g_sdram_test_actual         = 0;
volatile uint32_t g_sdram_test_passed         = 0;

/* 共享内存链接与读写测试诊断变量 */
volatile uint32_t g_shared_mem_test_address   = 0;
volatile uint32_t g_shared_mem_test_fail_index = 0;
volatile uint32_t g_shared_mem_test_expected  = 0;
volatile uint32_t g_shared_mem_test_actual    = 0;
volatile uint32_t g_shared_mem_test_passed    = 0;

/*
 * 测试对象：验证.shared_mem输入段是否被链接到共享SDRAM起始地址。
 * 对象大小为64字节，并按32字节边界对齐。
 */
static volatile shared_mem_control_block_t g_shared_mem_test_block
    BSP_PLACE_IN_SECTION(".shared_mem")
    BSP_ALIGN_VARIABLE(32);

static void cpu0_ipc_error_wait(fsp_err_t err)
{
    g_cpu0_ipc_last_error = err;

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/*
 * IPC中断回调。
 * 这里只取出32位消息并唤醒任务，不进行回复、打印或复杂计算。
 */
void cpu0_ipc_callback(ipc_callback_args_t * p_args)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    if (NULL == p_args)
    {
        return;
    }

    g_cpu0_ipc_last_event = (uint32_t) p_args->event;

    if (IPC_EVENT_MESSAGE_RECEIVED == p_args->event)
    {
        g_cpu0_ipc_last_rx = p_args->message;
        g_cpu0_ipc_rx_count++;

        if (NULL != s_cpu0_ipc_task)
        {
            (void) xTaskNotifyFromISR(s_cpu0_ipc_task,
                                     p_args->message,
                                     eSetValueWithOverwrite,
                                     &higher_priority_task_woken);

            portYIELD_FROM_ISR(higher_priority_task_woken);
        }
    }
}

/*
 * 测试辅助函数：计算一个32位数据的CRC32。
 * 仅供SDRAM完整测试使用。
 */
static uint32_t cpu0_sdram_test_crc32_word(uint32_t crc,
                                          uint32_t data)
{
    for (uint32_t byte_index = 0;
         byte_index < sizeof(uint32_t);
         byte_index++)
    {
        crc ^= data & 0xFFUL;
        data >>= 8;

        for (uint32_t bit_index = 0;
             bit_index < 8UL;
             bit_index++)
        {
            if (0UL != (crc & 1UL))
            {
                crc = (crc >> 1) ^ 0xEDB88320UL;
            }
            else
            {
                crc >>= 1;
            }
        }
    }

    return crc;
}

/*
 * 测试辅助函数：产生可重复的伪随机测试数据。
 * 相同种子始终产生相同数据序列。
 */
static uint32_t cpu0_sdram_test_prng_next(uint32_t state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;

    return state;
}

/*
 * 测试函数：对0x68000000～0x68FFFFFF执行完整16 MiB破坏性测试。
 *
 * 测试内容：
 * 1. 0x00000000固定模式。
 * 2. 0xFFFFFFFF固定模式。
 * 3. 0xAAAAAAAA固定模式。
 * 4. 0x55555555固定模式。
 * 5. 地址即数据模式。
 * 6. 反地址模式。
 * 7. 伪随机数据写入和CRC32校验。
 *
 * 警告：
 * 本函数会覆盖整个16 MiB SDRAM，必须在CPU1启动前执行。
 * 当前阶段SDRAM中没有有效业务数据或链接对象，才允许执行。
 */
static bool cpu0_sdram_full_test(void)
{
    volatile uint32_t * const p_sdram =
        (volatile uint32_t *) 0x68000000UL;

    static const uint32_t fixed_patterns[] =
    {
        0x00000000UL,
        0xFFFFFFFFUL,
        0xAAAAAAAAUL,
        0x55555555UL
    };

    const uint32_t word_count =
        0x01000000UL / sizeof(uint32_t);

    uint32_t expected_crc;
    uint32_t actual_crc;
    uint32_t prng_state;

    g_sdram_test_failed_address = 0;
    g_sdram_test_expected       = 0;
    g_sdram_test_actual         = 0;
    g_sdram_test_passed         = 0;

    /*
     * 测试四种固定数据模式。
     */
    for (uint32_t pattern_index = 0;
         pattern_index <
         (sizeof(fixed_patterns) / sizeof(fixed_patterns[0]));
         pattern_index++)
    {
        const uint32_t expected =
            fixed_patterns[pattern_index];

        g_printf("[CPU0] SDRAM fixed pattern %08X\r\n",
                 (unsigned int) expected);

        for (uint32_t i = 0; i < word_count; i++)
        {
            p_sdram[i] = expected;
        }

        __DSB();

        for (uint32_t i = 0; i < word_count; i++)
        {
            const uint32_t actual = p_sdram[i];

            if (actual != expected)
            {
                g_sdram_test_failed_address =
                    (uint32_t) (uintptr_t) &p_sdram[i];

                g_sdram_test_expected = expected;
                g_sdram_test_actual   = actual;

                return false;
            }
        }
    }

    /*
     * 测试地址即数据模式。
     */
    g_printf("[CPU0] SDRAM address pattern\r\n");

    for (uint32_t i = 0; i < word_count; i++)
    {
        p_sdram[i] =
            (uint32_t) (uintptr_t) &p_sdram[i];
    }

    __DSB();

    for (uint32_t i = 0; i < word_count; i++)
    {
        const uint32_t expected =
            (uint32_t) (uintptr_t) &p_sdram[i];

        const uint32_t actual = p_sdram[i];

        if (actual != expected)
        {
            g_sdram_test_failed_address =
                (uint32_t) (uintptr_t) &p_sdram[i];

            g_sdram_test_expected = expected;
            g_sdram_test_actual   = actual;

            return false;
        }
    }

    /*
     * 测试反地址模式。
     */
    g_printf("[CPU0] SDRAM inverse address pattern\r\n");

    for (uint32_t i = 0; i < word_count; i++)
    {
        p_sdram[i] =
            ~((uint32_t) (uintptr_t) &p_sdram[i]);
    }

    __DSB();

    for (uint32_t i = 0; i < word_count; i++)
    {
        const uint32_t expected =
            ~((uint32_t) (uintptr_t) &p_sdram[i]);

        const uint32_t actual = p_sdram[i];

        if (actual != expected)
        {
            g_sdram_test_failed_address =
                (uint32_t) (uintptr_t) &p_sdram[i];

            g_sdram_test_expected = expected;
            g_sdram_test_actual   = actual;

            return false;
        }
    }

    /*
     * 写入伪随机数据，同时计算期望CRC32。
     */
    g_printf("[CPU0] SDRAM pseudo-random CRC32\r\n");

    expected_crc = 0xFFFFFFFFUL;
    prng_state   = 0x13579BDFUL;

    for (uint32_t i = 0; i < word_count; i++)
    {
        prng_state = cpu0_sdram_test_prng_next(prng_state);
        p_sdram[i] = prng_state;

        expected_crc =
            cpu0_sdram_test_crc32_word(expected_crc,
                                       prng_state);
    }

    __DSB();

    /*
     * 从SDRAM重新读取数据并计算实际CRC32。
     */
    actual_crc = 0xFFFFFFFFUL;

    for (uint32_t i = 0; i < word_count; i++)
    {
        actual_crc =
            cpu0_sdram_test_crc32_word(actual_crc,
                                       p_sdram[i]);
    }

    expected_crc ^= 0xFFFFFFFFUL;
    actual_crc   ^= 0xFFFFFFFFUL;

    if (actual_crc != expected_crc)
    {
        /*
         * CRC不一致时重新逐字定位第一处错误。
         */
        prng_state = 0x13579BDFUL;

        for (uint32_t i = 0; i < word_count; i++)
        {
            prng_state =
                cpu0_sdram_test_prng_next(prng_state);

            if (p_sdram[i] != prng_state)
            {
                g_sdram_test_failed_address =
                    (uint32_t) (uintptr_t) &p_sdram[i];

                g_sdram_test_expected = prng_state;
                g_sdram_test_actual   = p_sdram[i];

                return false;
            }
        }

        /*
         * 理论上只有CRC计算路径异常才会进入这里。
         */
        g_sdram_test_expected = expected_crc;
        g_sdram_test_actual   = actual_crc;

        return false;
    }

    g_sdram_test_passed = 1;

    g_printf("[CPU0] SDRAM CRC32=%08X\r\n",
             (unsigned int) actual_crc);

    return true;
}

/* 测试辅助函数：计算共享载荷的CRC32。 */
static uint32_t cpu0_shared_mem_test_crc32(volatile uint8_t const * p_data,
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
 * 测试函数：验证共享控制块的链接地址并发布CPU0初始消息。
 * 发布顺序为载荷和元数据、内存屏障、READY状态。
 */
static bool cpu0_shared_mem_link_test(void)
{
    const uintptr_t block_address =
        (uintptr_t) &g_shared_mem_test_block;

    g_shared_mem_test_address    = (uint32_t) block_address;
    g_shared_mem_test_fail_index = 0;
    g_shared_mem_test_expected   = 0;
    g_shared_mem_test_actual     = 0;
    g_shared_mem_test_passed     = 0;

    /* 验证对象位于2 MiB共享内存区域并满足32字节对齐 */
    if ((block_address < 0x68800000UL) ||
        ((block_address + sizeof(g_shared_mem_test_block)) > 0x68A00000UL) ||
        (0UL != (block_address & 0x1FUL)))
    {
        g_shared_mem_test_expected = 0x68800000UL;
        g_shared_mem_test_actual   = (uint32_t) block_address;

        return false;
    }

    /* 写入载荷和协议元数据，暂不发布READY状态 */
    g_shared_mem_test_block.state       = SHARED_MEM_STATE_FREE;
    g_shared_mem_test_block.magic       = SHARED_MEM_MAGIC;
    g_shared_mem_test_block.version     = SHARED_MEM_VERSION;
    g_shared_mem_test_block.owner       = SHARED_MEM_OWNER_CPU1;
    g_shared_mem_test_block.sequence    = 1UL;
    g_shared_mem_test_block.data_length = SHARED_MEM_PAYLOAD_SIZE;
    g_shared_mem_test_block.error_code  = 0UL;

    for (uint32_t i = 0; i < SHARED_MEM_PAYLOAD_SIZE; i++)
    {
        g_shared_mem_test_block.payload[i] =
            (uint8_t) ((i * 37UL) ^ 0xA5UL);
    }

    g_shared_mem_test_block.crc32 =
        cpu0_shared_mem_test_crc32(g_shared_mem_test_block.payload,
                                   SHARED_MEM_PAYLOAD_SIZE);

    /* READY最后发布，CPU1看到READY后才能读取其他字段 */
    __DMB();
    g_shared_mem_test_block.state = SHARED_MEM_STATE_READY;
    __DSB();

    g_shared_mem_test_passed = 1;

    return true;
}

/*
 * 测试函数：CPU0验证CPU1写回的协议字段、CRC32和响应载荷。
 * CPU1完成写入并执行内存屏障后，以首个PING作为数据就绪通知。
 */
static bool cpu0_shared_mem_peer_response_test(void)
{
    /* 保证先观察到IPC事件，再读取CPU1已经发布的共享数据 */
    __DMB();

    if ((g_shared_mem_test_block.magic != SHARED_MEM_MAGIC) ||
        (g_shared_mem_test_block.version != SHARED_MEM_VERSION) ||
        (g_shared_mem_test_block.state != SHARED_MEM_STATE_DONE) ||
        (g_shared_mem_test_block.owner != SHARED_MEM_OWNER_CPU0) ||
        (g_shared_mem_test_block.sequence != 1UL) ||
        (g_shared_mem_test_block.data_length != SHARED_MEM_PAYLOAD_SIZE) ||
        (g_shared_mem_test_block.error_code != 0UL))
    {
        g_shared_mem_test_expected = SHARED_MEM_STATE_DONE;
        g_shared_mem_test_actual   = g_shared_mem_test_block.state;

        return false;
    }

    const uint32_t actual_crc =
        cpu0_shared_mem_test_crc32(g_shared_mem_test_block.payload,
                                   g_shared_mem_test_block.data_length);

    if (actual_crc != g_shared_mem_test_block.crc32)
    {
        g_shared_mem_test_expected = g_shared_mem_test_block.crc32;
        g_shared_mem_test_actual   = actual_crc;

        return false;
    }

    for (uint32_t i = 0; i < SHARED_MEM_PAYLOAD_SIZE; i++)
    {
        const uint8_t expected =
            (uint8_t) ((i * 53UL) ^ 0x5AUL);

        const uint8_t actual =
            g_shared_mem_test_block.payload[i];

        if (actual != expected)
        {
            g_shared_mem_test_fail_index = i;
            g_shared_mem_test_expected   = expected;
            g_shared_mem_test_actual     = actual;

            return false;
        }
    }

    /* CPU0消费完成后收回所有权并最后发布FREE状态 */
    g_shared_mem_test_block.owner = SHARED_MEM_OWNER_CPU0;
    __DMB();
    g_shared_mem_test_block.state = SHARED_MEM_STATE_FREE;
    __DSB();

    return true;
}

/*
 * CPU0 IPC线程入口。
 * 形参由FSP生成的FreeRTOS线程包装函数传入，本阶段不使用。
 */
void ipc_thread_entry(void * pvParameters)
{
    fsp_err_t err;
    uint32_t received_message;
    uint32_t reply_message;

    FSP_PARAMETER_NOT_USED(pvParameters);

    SEGGER_RTT_Init();
    g_printf("[CPU0] RTT initialized\r\n");

    if (cpu0_sdram_full_test())
    {
        g_printf("[CPU0] SDRAM full 16 MiB test PASS\r\n");
    }
    else
    {
        g_printf("[CPU0] SDRAM FAIL ADDR=%08X EXP=%08X ACT=%08X\r\n",
                (unsigned int) g_sdram_test_failed_address,
                (unsigned int) g_sdram_test_expected,
                (unsigned int) g_sdram_test_actual);

        while (1)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    if (cpu0_shared_mem_link_test())
    {
        g_printf("[CPU0] SHARED_MEM PASS ADDR=%08X SIZE=%u\r\n",
                 (unsigned int) g_shared_mem_test_address,
                 (unsigned int) sizeof(g_shared_mem_test_block));
    }
    else
    {
        g_printf("[CPU0] SHARED_MEM FAIL ADDR=%08X INDEX=%u EXP=%02X ACT=%02X\r\n",
                 (unsigned int) g_shared_mem_test_address,
                 (unsigned int) g_shared_mem_test_fail_index,
                 (unsigned int) g_shared_mem_test_expected,
                 (unsigned int) g_shared_mem_test_actual);

        while (1)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    /* 测试调用：CPU1启动前读取OV5640芯片ID，不启动VIN或视频流。 */
    if (camera_i2c_id_test())
    {
        g_printf("[CPU0] OV5640 I2C ID PASS ID=%04X\r\n",
                 (unsigned int) g_camera_i2c_test_chip_id);
    }
    else
    {
        g_printf("[CPU0] OV5640 I2C ID FAIL ID=%04X FSP_ERR=%u\r\n",
                 (unsigned int) g_camera_i2c_test_chip_id,
                 (unsigned int) g_camera_i2c_test_last_error);

        while (1)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    g_printf("[CPU0] Starting CPU1\r\n");
    R_BSP_SecondaryCoreStart();
    g_printf("[CPU0] CPU1 start requested\r\n");

    s_cpu0_ipc_task = xTaskGetCurrentTaskHandle();

    /*
     * 先打开接收通道，使CPU0具备接收中断能力。
     * CPU0通过channel 0接收CPU1消息。
     */
    err = g_ipc0.p_api->open(g_ipc0.p_ctrl, g_ipc0.p_cfg);
    if (FSP_SUCCESS != err)
    {
        cpu0_ipc_error_wait(err);
    }

    /*
     * 再打开发送通道。
     * CPU0通过channel 1向CPU1发送消息。
     */
    err = g_ipc1.p_api->open(g_ipc1.p_ctrl, g_ipc1.p_cfg);
    if (FSP_SUCCESS != err)
    {
        cpu0_ipc_error_wait(err);
    }

    while (1)
    {
        received_message = 0;

        if (pdTRUE == xTaskNotifyWait(0,
                                      0xFFFFFFFFUL,
                                      &received_message,
                                      portMAX_DELAY))
        {
            if ((received_message & IPC_MSG_TYPE_MASK) == IPC_MSG_PING)
            {
                if (!s_cpu0_shared_peer_checked)
                {
                    if (cpu0_shared_mem_peer_response_test())
                    {
                        s_cpu0_shared_peer_checked = true;

                        g_printf("[CPU0] CPU1 SHARED_MEM response PASS\r\n");
                    }
                    else
                    {
                        g_printf("[CPU0] CPU1 SHARED_MEM response FAIL "
                                 "INDEX=%u EXP=%02X ACT=%02X\r\n",
                                 (unsigned int) g_shared_mem_test_fail_index,
                                 (unsigned int) g_shared_mem_test_expected,
                                 (unsigned int) g_shared_mem_test_actual);

                        while (1)
                        {
                            vTaskDelay(pdMS_TO_TICKS(1000));
                        }
                    }
                }

                reply_message = IPC_MSG_ACK |
                                (received_message & IPC_MSG_COUNTER_MASK);

                err = g_ipc1.p_api->messageSend(g_ipc1.p_ctrl,
                                                reply_message);

                g_cpu0_ipc_last_error = err;

                if (FSP_SUCCESS == err)
                {
                    g_cpu0_ipc_last_tx = reply_message;
                    g_cpu0_ipc_tx_count++;

                    g_printf("[CPU0] RX=%08X TX=%08X RX_CNT=%u TX_CNT=%u "
                             "PROTO_ERR=%u FSP_ERR=%u STACK_OVF=%u\r\n",
                             (unsigned int) g_cpu0_ipc_last_rx,
                             (unsigned int) g_cpu0_ipc_last_tx,
                             (unsigned int) g_cpu0_ipc_rx_count,
                             (unsigned int) g_cpu0_ipc_tx_count,
                             (unsigned int) g_cpu0_ipc_protocol_error,
                             (unsigned int) g_cpu0_ipc_last_error,
                             (unsigned int) g_stack_overflow_count);
                }
            }
            else
            {
                g_cpu0_ipc_protocol_error++;
            }
        }
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
