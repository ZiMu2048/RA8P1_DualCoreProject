#include "camera_thread.h"
#include "Camera/i2c_control.h"
#include "Camera/camera_sensor.h"
#include "common/common.h"
#include "SEGGER_RTT/bsp_print.h"

#include <string.h> /*正式版删除*/

static volatile fsp_err_t g_camera_init_error = FSP_SUCCESS;

/*正式版删除：MIPI-CSI/VIN单帧采集测试状态*/
volatile uint32_t g_camera_frame_complete_count = 0U;
volatile uint32_t g_camera_vin_error_count      = 0U;
volatile uint32_t g_camera_mipi_event_count     = 0U;
volatile uint32_t g_camera_event_post_fail_count = 0U;
volatile uint32_t g_camera_vin_last_event_status = 0U;
volatile uint32_t g_camera_vin_last_interrupt_status = 0U;
volatile uint32_t g_camera_mipi_frame_data_count = 0U;
volatile uint32_t g_camera_mipi_data_lane_count = 0U;
volatile uint32_t g_camera_mipi_virtual_channel_count = 0U;
volatile uint32_t g_camera_mipi_power_count = 0U;
volatile uint32_t g_camera_mipi_short_packet_count = 0U;
volatile uint32_t g_camera_mipi_last_status = 0U;
volatile uint8_t g_camera_mipi_last_event_index = 0U;
uint8_t * volatile gp_camera_completed_buffer  = NULL;
volatile mipi_csi_event_t g_camera_mipi_last_event = MIPI_CSI_EVENT_FRAME_DATA;

/*
 *[@name] vin_callback
 *[@type] function
 *[@usage] 记录VIN单帧完成状态并通知Camera Thread
 *[@argument] p_args
 *[@return] none
 *正式版删除
 */
void vin_callback(capture_callback_args_t * p_args)
{
    BaseType_t x_higher_priority_task_woken = pdFALSE;
    BaseType_t x_result;

    if (NULL == p_args)
    {
        return;
    }

    g_camera_vin_last_event_status = p_args->event_status;
    g_camera_vin_last_interrupt_status = p_args->interrupt_status;

    if (VIN_EVENT_ERROR == p_args->event)
    {
        g_camera_vin_error_count++;
        return;
    }

    if (VIN_EVENT_NOTIFY != p_args->event)
    {
        return;
    }

    vin_interrupt_status_t const interrupt_status =
        (vin_interrupt_status_t) p_args->interrupt_status;

    if (!interrupt_status.bits.frame_complete)
    {
        return;
    }

    gp_camera_completed_buffer = p_args->p_buffer;
    g_camera_frame_complete_count++;
    x_result = xEventGroupSetBitsFromISR(g_ai_app_event,
                                         CAMERA_CAPTURE_COMPLETED,
                                         &x_higher_priority_task_woken);

    if (pdFAIL == x_result)
    {
        g_camera_event_post_fail_count++;
        return;
    }

    portYIELD_FROM_ISR(x_higher_priority_task_woken);
}

/*
 *[@name] mipi_csi0_callback
 *[@type] function
 *[@usage] 记录MIPI-CSI事件供单帧采集测试观察
 *[@argument] p_args
 *[@return] none
 *正式版删除
 */
void mipi_csi0_callback(mipi_csi_callback_args_t * p_args)
{
    if (NULL == p_args)
    {
        return;
    }

    g_camera_mipi_last_event = p_args->event;
    g_camera_mipi_last_event_index = p_args->event_idx;
    g_camera_mipi_event_count++;

    switch (p_args->event)
    {
        case MIPI_CSI_EVENT_FRAME_DATA:
        {
            g_camera_mipi_frame_data_count++;
            g_camera_mipi_last_status = p_args->event_data.receive_status.mask;
            break;
        }

        case MIPI_CSI_EVENT_DATA_LANE:
        {
            g_camera_mipi_data_lane_count++;
            g_camera_mipi_last_status = p_args->event_data.data_lane_status.mask;
            break;
        }

        case MIPI_CSI_EVENT_VIRTUAL_CHANNEL:
        {
            g_camera_mipi_virtual_channel_count++;
            g_camera_mipi_last_status = p_args->event_data.virtual_channel_status.mask;
            break;
        }

        case MIPI_CSI_EVENT_POWER:
        {
            g_camera_mipi_power_count++;
            g_camera_mipi_last_status = p_args->event_data.power_status.mask;
            break;
        }

        case MIPI_CSI_EVENT_SHORT_PACKET_FIFO:
        {
            g_camera_mipi_short_packet_count++;
            g_camera_mipi_last_status = p_args->event_data.fifo_status.mask;
            break;
        }

        default:
        {
            break;
        }
    }
}

/*
 *[@name] camera_single_frame_capture_test
 *[@type] static function
 *[@usage] 启动MIPI-CSI/VIN并完成一次1024x600 RGB565帧采集
 *[@argument] none
 *[@return] FSP error code
 *正式版删除
 */
static fsp_err_t camera_single_frame_capture_test(void)
{
    fsp_err_t err;
    EventBits_t events;
    uint8_t * p_completed_buffer;

    (void) xEventGroupClearBits(g_ai_app_event, CAMERA_CAPTURE_COMPLETED);
    gp_camera_completed_buffer = NULL;

    memset(vin_image_buffer_1, 0, VIN_BYTES_PER_FRAME);
    memset(vin_image_buffer_2, 0, VIN_BYTES_PER_FRAME);
    memset(vin_image_buffer_3, 0, VIN_BYTES_PER_FRAME);

#if BSP_CFG_DCACHE_ENABLED
    SCB_CleanInvalidateDCache_by_Addr((uint32_t *) vin_image_buffer_1,
                                      (int32_t) VIN_BYTES_PER_FRAME);
    SCB_CleanInvalidateDCache_by_Addr((uint32_t *) vin_image_buffer_2,
                                      (int32_t) VIN_BYTES_PER_FRAME);
    SCB_CleanInvalidateDCache_by_Addr((uint32_t *) vin_image_buffer_3,
                                      (int32_t) VIN_BYTES_PER_FRAME);
    __DMB();
#endif

    err = camera_stream_off();
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    err = R_VIN_Open(&g_vin_ctrl, &g_vin_cfg);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    err = R_VIN_CaptureStart(&g_vin_ctrl, NULL);
    if (FSP_SUCCESS != err)
    {
        (void) R_VIN_Close(&g_vin_ctrl);
        return err;
    }

    err = camera_stream_on();
    if (FSP_SUCCESS != err)
    {
        (void) R_VIN_Close(&g_vin_ctrl);
        return err;
    }

    events = xEventGroupWaitBits(g_ai_app_event,
                                 CAMERA_CAPTURE_COMPLETED,
                                 pdTRUE,
                                 pdFALSE,
                                 pdMS_TO_TICKS(2000U));

    (void) camera_stream_off();
    p_completed_buffer = gp_camera_completed_buffer;

    if (0U == (events & CAMERA_CAPTURE_COMPLETED))
    {
        g_printf("[CAM][ERR] VIN frame timeout: frames=%u, vin_errors=%u, mipi_events=%u, vin_status=0x%08X, vin_irq=0x%08X.\r\n",
                 (unsigned int) g_camera_frame_complete_count,
                 (unsigned int) g_camera_vin_error_count,
                 (unsigned int) g_camera_mipi_event_count,
                 (unsigned int) g_camera_vin_last_event_status,
                 (unsigned int) g_camera_vin_last_interrupt_status);
        (void) R_VIN_Close(&g_vin_ctrl);
        return FSP_ERR_TIMEOUT;
    }

    if (NULL == p_completed_buffer)
    {
        (void) R_VIN_Close(&g_vin_ctrl);
        return FSP_ERR_INTERNAL;
    }

#if BSP_CFG_DCACHE_ENABLED
    SCB_InvalidateDCache_by_Addr((uint32_t *) p_completed_buffer,
                                 (int32_t) VIN_BYTES_PER_FRAME);
    __DMB();
#endif

    g_printf("[CAM] VIN frame captured: count=%u, buffer=0x%08X, vin_errors=%u, mipi_events=%u.\r\n",
             (unsigned int) g_camera_frame_complete_count,
             (unsigned int) (uintptr_t) p_completed_buffer,
             (unsigned int) g_camera_vin_error_count,
             (unsigned int) g_camera_mipi_event_count);
    g_printf("[CAM] MIPI events: frame=%u, lane=%u, vc=%u, power=%u, short=%u, last_event=%u, last_index=%u, last_status=0x%08X.\r\n",
             (unsigned int) g_camera_mipi_frame_data_count,
             (unsigned int) g_camera_mipi_data_lane_count,
             (unsigned int) g_camera_mipi_virtual_channel_count,
             (unsigned int) g_camera_mipi_power_count,
             (unsigned int) g_camera_mipi_short_packet_count,
             (unsigned int) g_camera_mipi_last_event,
             (unsigned int) g_camera_mipi_last_event_index,
             (unsigned int) g_camera_mipi_last_status);

    err = R_VIN_Close(&g_vin_ctrl);
    return err;
}

/* Camera Thread entry function */
/* pvParameters contains TaskHandle_t */
void camera_thread_entry(void * pvParameters)
{
    FSP_PARAMETER_NOT_USED(pvParameters);

    g_printf("\r\n[CAM] Camera Thread started.\r\n");
    g_printf("[CAM] External active oscillator is used; P501 is not used.\r\n");

    g_printf("[CAM] Opening IIC master...\r\n");
    g_camera_init_error = i2c_control_init();
    if (FSP_SUCCESS != g_camera_init_error)
    {
        g_printf("[CAM][ERR] IIC open failed: %u\r\n", (unsigned int) g_camera_init_error);
        APP_ERROR_TRAP(g_camera_init_error);
        while (true)
        {
            vTaskDelay(pdMS_TO_TICKS(1000U));
        }
    }

    g_printf("[CAM] IIC master opened.\r\n");
    g_printf("[CAM] Initializing OV5640...\r\n");
    g_camera_init_error = camera_open();
    if (FSP_SUCCESS != g_camera_init_error)
    {
        g_printf("[CAM][ERR] OV5640 initialization failed: %u\r\n", (unsigned int) g_camera_init_error);
        APP_ERROR_TRAP(g_camera_init_error);
        while (true)
        {
            vTaskDelay(pdMS_TO_TICKS(1000U));
        }
    }

    xEventGroupSetBits(g_ai_app_event, HARDWARE_CAMERA_INIT_DONE);
    g_printf("[CAM] OV5640 initialization completed.\r\n");
    g_printf("[CAM] HARDWARE_CAMERA_INIT_DONE set.\r\n");

    /*正式版删除：MIPI-CSI/VIN单帧采集闭环测试*/
    g_printf("[CAM] Starting MIPI-CSI/VIN single-frame capture test.\r\n");
    g_camera_init_error = camera_single_frame_capture_test();
    if (FSP_SUCCESS != g_camera_init_error)
    {
        g_printf("[CAM][ERR] Single-frame capture failed: %u\r\n",
                 (unsigned int) g_camera_init_error);
        APP_ERROR_TRAP(g_camera_init_error);
    }
    else
    {
        g_printf("[CAM] Single-frame capture test completed.\r\n");
    }

    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}
