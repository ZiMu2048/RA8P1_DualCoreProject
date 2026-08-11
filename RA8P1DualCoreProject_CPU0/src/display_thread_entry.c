#include "display_thread.h"
#include "Display/glcdc_display.h"
#include "common/common.h"
#include "Camera/camera_capture.h"

volatile uint32_t g_glcdc_vsync_timeout_count = 0U;

/*
 *[@name] display_thread_entry
 *[@type] thread entry function
 *[@usage] 初始化GLCDC，接收Camera完成帧，复制到后台缓冲区并在行检测事件后安全换帧
 *[@argument] pvParameters FSP传入的线程参数，当前未使用
 *[@return] none
 */
void display_thread_entry(void * pvParameters)
{
    FSP_PARAMETER_NOT_USED(pvParameters);

    fsp_err_t err;
    uint8_t draw_buffer_index = 1U;
    uint32_t last_displayed_sequence = 0U;
    uint32_t completed_sequence = 0U;

    err = init_display();
    if (FSP_SUCCESS != err)
    {
        APP_ERROR_TRAP(err);
    }

    xEventGroupSetBits(g_ai_app_event, HARDWARE_DISPLAY_INIT_DONE);

    while(true)
    {
        EventBits_t events = xEventGroupWaitBits(g_ai_app_event,
                                                 CAMERA_FRAME_READY,
                                                 pdTRUE,
                                                 pdFALSE,
                                                 portMAX_DELAY);

        if (0U == (events & CAMERA_FRAME_READY))
        {
            continue;
        }

        uint8_t * p_completed_frame =
            camera_completed_frame_get(&completed_sequence);

        if ((NULL == p_completed_frame) || (completed_sequence == last_displayed_sequence))
        {
            continue;
        }

        err = display_camera_frame_copy(p_completed_frame,
                                        draw_buffer_index);
        if (FSP_SUCCESS != err)
        {
            APP_ERROR_TRAP(err);
        }

        for (;;)
        {
            err = display_wait_next_vsync(pdMS_TO_TICKS(50U));

            if (FSP_ERR_TIMEOUT == err)
            {
                g_glcdc_vsync_timeout_count++;
                continue;
            }

            if (FSP_SUCCESS != err)
            {
                APP_ERROR_TRAP(err);
            }

            err = R_GLCDC_BufferChange(
                &g_display_ctrl,
                (uint8_t *) fb_background[draw_buffer_index],
                DISPLAY_FRAME_LAYER_1);

            if (FSP_SUCCESS == err)
            {
                last_displayed_sequence = completed_sequence;
                draw_buffer_index ^= 1U;
                g_glcdc_swap_ok_count++;
                break;
            }

            if (FSP_ERR_INVALID_UPDATE_TIMING == err)
            {
                g_glcdc_invalid_timing_count++;
                continue;
            }

            APP_ERROR_TRAP(err);
        }
    }
}
