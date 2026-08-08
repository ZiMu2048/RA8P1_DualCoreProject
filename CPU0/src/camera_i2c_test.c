#include "camera_i2c_test.h"

#define CAMERA_I2C_TEST_SLAVE_ADDRESS       (0x3CU)
#define CAMERA_I2C_TEST_ID_HIGH_REGISTER    (0x300AU)
#define CAMERA_I2C_TEST_ID_LOW_REGISTER     (0x300BU)
#define CAMERA_I2C_TEST_EXPECTED_ID          (0x5640U)
#define CAMERA_I2C_TEST_RESET_PIN            (BSP_IO_PORT_07_PIN_09)
#define CAMERA_I2C_TEST_WAIT_COUNT           (100000UL)

/* 测试诊断变量：可在调试器Expressions窗口中直接观察。 */
volatile uint8_t   g_camera_i2c_test_id_high   = 0U;
volatile uint8_t   g_camera_i2c_test_id_low    = 0U;
volatile uint16_t  g_camera_i2c_test_chip_id   = 0U;
volatile fsp_err_t g_camera_i2c_test_last_error = FSP_SUCCESS;
volatile uint32_t  g_camera_i2c_test_passed    = 0U;

/* 测试同步事件：由IIC中断回调写入，由CPU0测试任务读取。 */
static volatile i2c_master_event_t s_camera_i2c_test_event = I2C_MASTER_EVENT_ABORTED;

/* 测试辅助函数：等待指定IIC完成事件，并限制最长等待时间。 */
static fsp_err_t camera_i2c_test_wait_event(i2c_master_event_t expected_event)
{
    uint32_t wait_count = CAMERA_I2C_TEST_WAIT_COUNT;

    while (expected_event != s_camera_i2c_test_event)
    {
        if (I2C_MASTER_EVENT_ABORTED == s_camera_i2c_test_event)
        {
            return FSP_ERR_TRANSFER_ABORTED;
        }

        if (0U == wait_count)
        {
            return FSP_ERR_TIMEOUT;
        }

        wait_count--;
        R_BSP_SoftwareDelay(10U, BSP_DELAY_UNITS_MICROSECONDS);
    }

    return FSP_SUCCESS;
}

/* 测试辅助函数：向OV5640发送16位寄存器地址并读取一个字节。 */
static fsp_err_t camera_i2c_test_read_register(uint16_t register_address,
                                                uint8_t * p_value)
{
    fsp_err_t err;
    uint8_t register_buffer[2];

    if (NULL == p_value)
    {
        return FSP_ERR_ASSERTION;
    }

    register_buffer[0] = (uint8_t) (register_address >> 8);
    register_buffer[1] = (uint8_t) register_address;

    s_camera_i2c_test_event = (i2c_master_event_t) 0;
    err = R_IIC_MASTER_Write(&g_i2c_master_for_peripheral_ctrl,
                             register_buffer,
                             sizeof(register_buffer),
                             true);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    err = camera_i2c_test_wait_event(I2C_MASTER_EVENT_TX_COMPLETE);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    s_camera_i2c_test_event = (i2c_master_event_t) 0;
    err = R_IIC_MASTER_Read(&g_i2c_master_for_peripheral_ctrl,
                            p_value,
                            1U,
                            false);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    return camera_i2c_test_wait_event(I2C_MASTER_EVENT_RX_COMPLETE);
}

/*
 * 测试函数：验证CPU0能够通过IIC1访问OV5640。
 * 测试流程为打开IIC、设置0x3C从地址、硬件复位、读取芯片ID并关闭IIC。
 */
bool camera_i2c_id_test(void)
{
    fsp_err_t err;
    fsp_err_t close_err;
    bool opened = false;

    g_camera_i2c_test_id_high    = 0U;
    g_camera_i2c_test_id_low     = 0U;
    g_camera_i2c_test_chip_id    = 0U;
    g_camera_i2c_test_last_error = FSP_SUCCESS;
    g_camera_i2c_test_passed     = 0U;

    err = R_IIC_MASTER_Open(&g_i2c_master_for_peripheral_ctrl,
                            &g_i2c_master_for_peripheral_cfg);
    if (FSP_SUCCESS != err)
    {
        g_camera_i2c_test_last_error = err;
        return false;
    }
    opened = true;

    err = R_IIC_MASTER_SlaveAddressSet(&g_i2c_master_for_peripheral_ctrl,
                                       CAMERA_I2C_TEST_SLAVE_ADDRESS,
                                       I2C_MASTER_ADDR_MODE_7BIT);

    if (FSP_SUCCESS == err)
    {
        /* 测试复位时序：保持复位低电平20 ms，再释放并等待20 ms。 */
        R_BSP_PinAccessEnable();
        R_BSP_PinWrite(CAMERA_I2C_TEST_RESET_PIN, BSP_IO_LEVEL_LOW);
        R_BSP_PinAccessDisable();
        R_BSP_SoftwareDelay(20U, BSP_DELAY_UNITS_MILLISECONDS);

        R_BSP_PinAccessEnable();
        R_BSP_PinWrite(CAMERA_I2C_TEST_RESET_PIN, BSP_IO_LEVEL_HIGH);
        R_BSP_PinAccessDisable();
        R_BSP_SoftwareDelay(20U, BSP_DELAY_UNITS_MILLISECONDS);

        err = camera_i2c_test_read_register(CAMERA_I2C_TEST_ID_HIGH_REGISTER,
                                            (uint8_t *) &g_camera_i2c_test_id_high);
    }

    if (FSP_SUCCESS == err)
    {
        err = camera_i2c_test_read_register(CAMERA_I2C_TEST_ID_LOW_REGISTER,
                                            (uint8_t *) &g_camera_i2c_test_id_low);
    }

    g_camera_i2c_test_chip_id =
        (uint16_t) (((uint16_t) g_camera_i2c_test_id_high << 8) |
                    (uint16_t) g_camera_i2c_test_id_low);

    if (opened)
    {
        close_err = R_IIC_MASTER_Close(&g_i2c_master_for_peripheral_ctrl);
        if ((FSP_SUCCESS == err) && (FSP_SUCCESS != close_err))
        {
            err = close_err;
        }
    }

    g_camera_i2c_test_last_error = err;

    if ((FSP_SUCCESS == err) &&
        (CAMERA_I2C_TEST_EXPECTED_ID == g_camera_i2c_test_chip_id))
    {
        g_camera_i2c_test_passed = 1U;
        return true;
    }

    return false;
}

/*
 * 测试回调：运行在IIC中断上下文中，只保存事件，不阻塞、不打印且不调用FreeRTOS API。
 * 正式迁移IIC服务层时，此回调应由服务层接管。
 */
void g_i2c_master_for_peripheral_callback(i2c_master_callback_args_t * p_args)
{
    if (NULL != p_args)
    {
        s_camera_i2c_test_event = p_args->event;
    }
}
