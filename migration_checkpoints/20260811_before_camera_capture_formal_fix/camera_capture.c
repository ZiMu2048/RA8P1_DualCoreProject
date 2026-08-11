/*
 * camera_capture.c
 *
 *  Created on: 2026年8月11日
 *      Author: lingk
 */
#include "hal_data.h"
#include "Camera/camera_sensor.h"
#include "Camera/camera_capture.h"

static uint8_t * volatile gp_camera_completed_frame; //帧完成缓冲区地址
static volatile uint32_t g_camera_frame_sequence;    //帧计数
static uint32_t g_camera_vin_error_count;            //错误计数

fsp_err_t camera_capture_open(void)
{
    fsp_err_t err = FSP_SUCCESS;

    gp_camera_completed_frame = NULL;
    g_camera_frame_sequence = 0U;
    g_camera_vin_error_count = 0U;

#if BSP_CFG_DCACHE_ENABLED
    SCB_CleanInvalidateDCache_by_Addr(
        (uint32_t *) vin_image_buffer_1,
        (int32_t) VIN_BYTES_PER_FRAME);

    SCB_CleanInvalidateDCache_by_Addr(
        (uint32_t *) vin_image_buffer_2,
        (int32_t) VIN_BYTES_PER_FRAME);
    SCB_CleanInvalidateDCache_by_Addr(
        (uint32_t *) vin_image_buffer_3,
        (int32_t) VIN_BYTES_PER_FRAME);
#endif

    __DMB();

    err = R_VIN_Open(&g_vin_ctrl, &g_vin_cfg);
    if(FSP_SUCCESS != err)
    {
        return err;
    }

    return FSP_SUCCESS;
}

fsp_err_t camera_capture_start(void)
{
    fsp_err_t err = FSP_SUCCESS;

    err = R_VIN_CaptureStart(&g_vin_ctrl, NULL);
    if(FSP_SUCCESS != err)
    {
        return err;
    }

    err = camera_stream_on();
    if(FSP_SUCCESS != err)
    {
	R_VIN_Close(&g_vin_ctrl);
        return err;
    }

    return FSP_SUCCESS;
}

fsp_err_t camera_capture_stop(void)
{
    fsp_err_t err = FSP_SUCCESS;

    err = camera_stream_off();
    if(FSP_SUCCESS != err)
    {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(40U));

    err = R_VIN_Close(&g_vin_ctrl);
    if(FSP_SUCCESS != err)
    {
        return err;
    }

    return FSP_SUCCESS;
}

uint8_t * camera_completed_frame_get(uint32_t * p_sequence)
{
    if(NULL == p_sequence)
    {
        return NULL;
    }

    *p_sequence = g_camera_frame_sequence;
    g_camera_frame_sequence++;

    return gp_camera_completed_frame;
}

void vin_callback(capture_callback_args_t *p_args)
{
	vin_interrupt_status_t interrupt_status =
			(vin_interrupt_status_t) p_args->interrupt_status;

    switch (p_args->event)
    {
        case VIN_EVENT_NOTIFY:
        {
            if (interrupt_status.bits.frame_complete)
            {
	            	gp_camera_completed_frame = p_args->p_buffer;
	            	g_camera_frame_sequence++;
	            	__DMB();
	            	xEventGroupSetBits(CAMERA_FRAME_READY, true);
            }

            break;
        }

        case VIN_EVENT_ERROR:
        {
	        	g_camera_vin_error_count++;
	        	__DMB();
	        	xEventGroupSetBits(CAMERA_CAPTURE_ERROR, true);
            break;
        }

        default:
        {
            /* Do nothing */
            break;
        }
    }
}

void mipi_csi0_callback(mipi_csi_callback_args_t * p_args)
{
    switch (p_args->event)
    {

        case MIPI_CSI_EVENT_DATA_LANE:
        case MIPI_CSI_EVENT_FRAME_DATA:
        case MIPI_CSI_EVENT_POWER:
        case MIPI_CSI_EVENT_SHORT_PACKET_FIFO:
        case MIPI_CSI_EVENT_VIRTUAL_CHANNEL:
            break;

        default:
            break;
    }
}
