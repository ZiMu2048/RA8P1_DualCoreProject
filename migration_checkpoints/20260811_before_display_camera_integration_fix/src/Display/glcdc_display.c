/*
 * glcdc_display.c
 *
 *  Created on: 2026年8月10日
 *      Author: lingk
 */
#include "glcdc_display.h"
#include "common/common.h"
#include <string.h>
/*
 *[@type] global variable
 *[@usage] GLCDC运行状态
 */
volatile uint32_t g_glcdc_vsync_count          = 0U;
volatile uint32_t g_glcdc_gr1_underflow_count  = 0U;
volatile uint32_t g_glcdc_swap_ok_count        = 0U;
volatile uint32_t g_glcdc_invalid_timing_count = 0U;

/*
 *[@name] glcdc_vsync_isr
 *[@type] function
 *[@usage] GLCDC Interrupt Callback
 *[@argument] p_args
 *[@return] none
 */
void glcdc_vsync_isr(display_callback_args_t * p_args)
{
    BaseType_t x_higher_priority_task_woken = pdFALSE;
    BaseType_t x_result;

    if (NULL == p_args)
    {
        return;
    }

    if (DISPLAY_EVENT_GR1_UNDERFLOW == p_args->event)
    {
        g_glcdc_gr1_underflow_count++;
        return;
    }

    if (DISPLAY_EVENT_LINE_DETECTION != p_args->event)
    {
        return;
    }

    g_glcdc_vsync_count++;
    x_result = xEventGroupSetBitsFromISR(g_ai_app_event,
                                         GLCDC_VSYNC,
                                         &x_higher_priority_task_woken);

    if (pdFAIL == x_result)
    {
        return;
    }

    portYIELD_FROM_ISR(x_higher_priority_task_woken);
}

/*
 *[@name] init_display
 *[@type] function
 *[@usage] GLCDC Display Initialization
 *[@argument] none
 *[@return] FSP error code
 */
fsp_err_t init_display(void)
{
    fsp_err_t err;

    err = R_GLCDC_Open(&g_display_ctrl, &g_display_cfg);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    err = R_GLCDC_Start(&g_display_ctrl);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    err = R_GLCDC_Start(&g_display_ctrl);
    if (FSP_SUCCESS != err)
    {
        (void) R_GLCDC_Close(&g_display_ctrl);
        return err;
    }

    return FSP_SUCCESS;
}

/*
 *[@name] display_camera_frame_copy
 *[@type] function
 *[@usage] 将VIN完成帧复制到指定GLCDC后台缓冲区，并完成源与目标缓冲区的Cache维护
 *[@argument] p_source VIN最近完成写入的帧缓冲区地址
 *[@argument] draw_buffer_index GLCDC后台缓冲区索引，只允许0或1
 *[@return] 成功返回FSP_SUCCESS，参数无效时返回对应FSP错误码
 */
fsp_err_t display_camera_frame_copy(uint8_t const * p_source, uint8_t draw_buffer_index)
{
	if ((NULL == p_source) || (draw_buffer_index > 1U))
	{
	    return FSP_ERR_INVALID_ARGUMENT;
	}

#if BSP_CFG_DCACHE_ENABLED
	SCB_InvalidateDCache_by_Addr(
	    (uint32_t *) p_source,
	    (int32_t) VIN_BYTES_PER_FRAME);
#endif

	memcpy(&fb_background[draw_buffer_index][0],
	       p_source,
	       VIN_BYTES_PER_FRAME);

#if BSP_CFG_DCACHE_ENABLED
	SCB_CleanDCache_by_Addr(
	    (uint32_t *) &fb_background[draw_buffer_index][0],
	    (int32_t) VIN_BYTES_PER_FRAME);
#endif

	__DMB();

	return FSP_SUCCESS;
}

/*
 *[@name] display_wait_next_vsync
 *[@type] function
 *[@usage] 清除旧同步事件并阻塞等待新的GLCDC行检测事件，只能在Display Thread中调用
 *[@argument] timeout 最大等待时间，单位为FreeRTOS Tick
 *[@return] 收到同步事件返回FSP_SUCCESS，超时返回FSP_ERR_TIMEOUT
 */
fsp_err_t display_wait_next_vsync(TickType_t timeout)
{
	EventBits_t events;

	xEventGroupClearBits(g_ai_app_event, GLCDC_VSYNC);

	events = xEventGroupWaitBits(g_ai_app_event,
	                             GLCDC_VSYNC,
	                             pdTRUE,
	                             pdFALSE,
	                             timeout);

    if (0U != (events & GLCDC_VSYNC))
    {
        return FSP_SUCCESS;
    }

	return FSP_ERR_TIMEOUT;
}




