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
extern volatile uint32_t g_glcdc_vsync_timeout_count;

/*
 *[@name] init_display
 *[@type] function
 *[@usage] 初始化并启动GLCDC，只能在Display Thread任务上下文调用
 *[@argument] none
 *[@return] 成功返回FSP_SUCCESS，失败返回对应FSP错误码
 */
fsp_err_t init_display(void);

/*
 *[@name] glcdc_vsync_isr
 *[@type] callback function
 *[@usage] GLCDC中断回调，由FSP在中断上下文调用
 *[@argument] p_args FSP传入的GLCDC事件参数
 *[@return] none
 */
void glcdc_vsync_isr(display_callback_args_t * p_args);

/*
 *[@name] display_camera_frame_copy
 *[@type] function
 *[@usage] 将完整VIN帧复制到指定GLCDC后台缓冲区，不可在中断上下文调用
 *[@argument] p_source VIN完成帧地址
 *[@argument] draw_buffer_index GLCDC后台缓冲区索引，只允许0或1
 *[@return] 成功返回FSP_SUCCESS，参数无效返回FSP_ERR_INVALID_ARGUMENT
 */
fsp_err_t display_camera_frame_copy(uint8_t const * p_source,
                                    uint8_t draw_buffer_index);

/*
 *[@name] display_wait_next_vsync
 *[@type] function
 *[@usage] 阻塞当前任务等待新的GLCDC行检测事件，不阻塞CPU且不可在中断中调用
 *[@argument] timeout 最大等待时间，单位为FreeRTOS Tick
 *[@return] 收到事件返回FSP_SUCCESS，超时返回FSP_ERR_TIMEOUT
 */
fsp_err_t display_wait_next_vsync(TickType_t timeout);

#endif /* DISPLAY_GLCDC_DISPLAY_H_ */
