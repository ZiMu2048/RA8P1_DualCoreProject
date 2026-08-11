#include "display_thread.h"
#include "Display/glcdc_display.h"
#include "common/common.h"
#include "Camera/camera_capture.h"
#include "Helium/helium_rgb565_resize.h"

/*
 * 摄像头源图像为1024×600。
 * 中央裁剪成1000×600后缩放为800×480，可以保持5:3宽高比。
 */
#define DISPLAY_SCALE_SOURCE_WIDTH          (1024U)
#define DISPLAY_SCALE_SOURCE_HEIGHT         (600U)
#define DISPLAY_SCALE_CROP_X                (12U)
#define DISPLAY_SCALE_CROP_Y                (0U)
#define DISPLAY_SCALE_CROP_WIDTH            (1000U)
#define DISPLAY_SCALE_CROP_HEIGHT           (600U)
#define DISPLAY_SCALE_DESTINATION_WIDTH     (800U)
#define DISPLAY_SCALE_DESTINATION_HEIGHT    (480U)

/*
 *[@type] global variable
 *[@usage] GLCDC等待行检测事件超时次数，供调试器观察
 */
volatile uint32_t g_glcdc_vsync_timeout_count = 0U;

/*
 *[@type] global variable
 *[@usage] Helium图像缩放失败次数，正常运行时应保持为0
 */
volatile uint32_t g_helium_resize_error_count = 0U;

/*
 *[@type] static variable
 *[@usage] Helium横向源像素索引工作区，只允许Display Thread访问
 */
static uint16_t
    g_display_resize_horizontal_map[DISPLAY_SCALE_DESTINATION_WIDTH]
    BSP_ALIGN_VARIABLE(16);

/*
 *[@name] display_thread_entry
 *[@type] thread entry function
 *[@usage] 初始化GLCDC，接收Camera完成帧，使用Helium缩放到后台缓冲区并安全换帧
 *[@argument] pvParameters FSP传入的线程参数，当前未使用
 *[@return] none
 */
void display_thread_entry(void * pvParameters)
{
    fsp_err_t err;
    helium_rgb565_resize_status_t resize_status;

    uint8_t draw_buffer_index = 1U;
    uint32_t last_displayed_sequence = 0U;
    uint32_t completed_sequence = 0U;

    FSP_PARAMETER_NOT_USED(pvParameters);

    /*
     * 初始化并启动GLCDC。
     * GLCDC初始扫描fb_background[0]，
     * 因此Display Thread首次绘制fb_background[1]。
     */
    err = init_display();
    if (FSP_SUCCESS != err)
    {
        APP_ERROR_TRAP(err);
    }

    /*
     * 通知Camera Thread显示硬件已经初始化完成。
     */
    xEventGroupSetBits(g_ai_app_event,
                       HARDWARE_DISPLAY_INIT_DONE);

    while (true)
    {
        EventBits_t events;

        /*
         * 等待Camera ISR发布新的完成帧。
         * pdTRUE表示返回前清除CAMERA_FRAME_READY事件位。
         */
        events = xEventGroupWaitBits(g_ai_app_event,
                                     CAMERA_FRAME_READY,
                                     pdTRUE,
                                     pdFALSE,
                                     portMAX_DELAY);

        if (0U == (events & CAMERA_FRAME_READY))
        {
            continue;
        }

        /*
         * 在临界区保护下取得帧地址和对应序号的一致快照。
         */
        uint8_t * p_completed_frame =
            camera_completed_frame_get(&completed_sequence);

        if ((NULL == p_completed_frame) ||
            (completed_sequence == last_displayed_sequence))
        {
            continue;
        }

        /*
         * 本配置描述一次1024×600 RGB565到800×480 RGB565的缩放。
         *
         * VIN和GLCDC缓冲区的实际行跨度均为1024个RGB565像素。
         * 缩放结果写入GLCDC后台缓冲区每行前800个像素。
         */
        helium_rgb565_resize_cfg_t const resize_cfg =
        {
            .p_source =
                (uint16_t const *) p_completed_frame,

            .source_width =
                DISPLAY_SCALE_SOURCE_WIDTH,

            .source_height =
                DISPLAY_SCALE_SOURCE_HEIGHT,

            .source_stride_pixels =
                VIN_CFG_IMAGE_STRIDE,

            .crop_x =
                DISPLAY_SCALE_CROP_X,

            .crop_y =
                DISPLAY_SCALE_CROP_Y,

            .crop_width =
                DISPLAY_SCALE_CROP_WIDTH,

            .crop_height =
                DISPLAY_SCALE_CROP_HEIGHT,

            .p_destination =
                (uint16_t *)
                &fb_background[draw_buffer_index][0],

            .destination_width =
                DISPLAY_SCALE_DESTINATION_WIDTH,

            .destination_height =
                DISPLAY_SCALE_DESTINATION_HEIGHT,

            .destination_stride_pixels =
                DISPLAY_BUFFER_STRIDE_PIXELS_INPUT0
        };

#if BSP_CFG_DCACHE_ENABLED

        /*
         * VIN通过DMA直接向SDRAM写入图像，不会更新CPU D-Cache。
         *
         * CPU读取VIN完成帧之前，必须丢弃Cache中可能存在的旧副本。
         */
        SCB_InvalidateDCache_by_Addr(
            (uint32_t *) p_completed_frame,
            (int32_t) VIN_BYTES_PER_FRAME);

#endif

        /*
         * 在任务上下文中执行Helium最近邻缩放。
         *
         * 函数内部没有全局状态、FreeRTOS调用或Cache操作。
         * 横向映射工作区由Display Thread独占。
         */
        resize_status = helium_rgb565_resize_nearest(
            &resize_cfg,
            g_display_resize_horizontal_map,
            sizeof(g_display_resize_horizontal_map) /
            sizeof(g_display_resize_horizontal_map[0]));

        if (HELIUM_RGB565_RESIZE_SUCCESS != resize_status)
        {
            g_helium_resize_error_count++;
            APP_ERROR_TRAP(FSP_ERR_INTERNAL);
        }

#if BSP_CFG_DCACHE_ENABLED

        /*
         * CPU通过Cache写入GLCDC后台缓冲区。
         *
         * GLCDC直接从SDRAM读取，因此换帧前必须把CPU修改的内容写回SDRAM。
         *
         * 第一版清理完整后台缓冲区，虽然范围略大，但最容易验证正确性。
         */
        SCB_CleanDCache_by_Addr(
            (uint32_t *)
            &fb_background[draw_buffer_index][0],
            (int32_t)
            sizeof(fb_background[draw_buffer_index]));

#endif

        /*
         * 确保图像数据和Cache维护先完成，
         * 然后再允许后续GLCDC换帧操作发生。
         */
        __DMB();

        for (;;)
        {
            err = display_wait_next_vsync(
                pdMS_TO_TICKS(50U));

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
                (uint8_t *)
                fb_background[draw_buffer_index],
                DISPLAY_FRAME_LAYER_1);

            if (FSP_SUCCESS == err)
            {
                /*
                 * 只有GLCDC接受新的后台缓冲区后，
                 * 才记录帧序号并切换下一次绘制目标。
                 */
                last_displayed_sequence =
                    completed_sequence;

                draw_buffer_index ^= 1U;
                g_glcdc_swap_ok_count++;

                break;
            }

            if (FSP_ERR_INVALID_UPDATE_TIMING == err)
            {
                /*
                 * 当前不在GLCDC允许的地址更新时间窗口。
                 * 保留同一个后台缓冲区，等待下一次行检测后重试。
                 */
                g_glcdc_invalid_timing_count++;
                continue;
            }

            APP_ERROR_TRAP(err);
        }
    }
}
