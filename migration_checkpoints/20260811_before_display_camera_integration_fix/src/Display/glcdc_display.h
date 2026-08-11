/*
 * glcdc_display.h
 *
 *  Created on: 2026年8月10日
 *      Author: lingk
 */
#ifndef DISPLAY_GLCDC_DISPLAY_H_
#define DISPLAY_GLCDC_DISPLAY_H_

#include "hal_data.h"

/*
 *[@type] global variable
 *[@usage] GLCDC运行状态，供调试器观察
 */
extern volatile uint32_t g_glcdc_vsync_count;
extern volatile uint32_t g_glcdc_gr1_underflow_count;
extern volatile uint32_t g_glcdc_swap_ok_count;
extern volatile uint32_t g_glcdc_invalid_timing_count;

/* 只能在Display Thread任务上下文调用 */
fsp_err_t init_display(void);

/* GLCDC中断回调，由FSP调用 */
void glcdc_vsync_isr(display_callback_args_t * p_args);
fsp_err_t display_camera_frame_copy(uint8_t const * p_source,
                                    uint8_t draw_buffer_index);
fsp_err_t display_wait_next_vsync(TickType_t timeout);

#endif /* DISPLAY_GLCDC_DISPLAY_H_ */
