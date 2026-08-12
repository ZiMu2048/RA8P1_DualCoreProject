#include "ai_thread.h"
#include "common/common.h"
#include "Camera/camera_capture.h"
#include "AI/ai_preprocess.h"
#include "AI/yolo_postprocess.h"
#include "model/model.h"
#include "SEGGER_RTT/bsp_print.h"
#include "AI/ai_inference_result.h"
#include <stddef.h>
#include <stdint.h>

#define AI_CONFIDENCE_THRESHOLD       (0.40f) /*最低检测置信度阈值*/
#define AI_NMS_IOU_THRESHOLD          (0.45f)

/*
 *[@name] g_ai_preprocess_horizontal_map
 *[@type] static global variable
 *[@usage] 保存Helium预处理使用的横向源像素索引，只允许AI Thread访问
 */
static uint16_t
    g_ai_preprocess_horizontal_map[AI_PREPROCESS_HORIZONTAL_MAP_LENGTH]
    BSP_ALIGN_VARIABLE(16);

/*
 *[@name] ai_thread_fatal_stop
 *[@type] static function
 *[@usage] 触发CPU0断点，调试器继续执行后使AI Thread保持阻塞
 *[@argument] error 需要记录和触发断点的FSP错误码
 *[@return] none，此函数不会返回
 */
static void ai_thread_fatal_stop(fsp_err_t error)
{
    APP_ERROR_TRAP(error);

    for(;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

/*
 *[@name] ai_thread_entry
 *[@type] thread entry function
 *[@usage] 初始化Ethos-U55与模型接口，等待Camera完成帧，执行Helium预处理、模型推理和YOLO后处理
 *[@argument] pvParameters FSP传入的线程参数，当前未使用
 *[@return] none
 */
void ai_thread_entry(void * pvParameters)
{
    fsp_err_t err;
    EventBits_t events;

    int8_t * p_model_input;
    int8_t * p_model_output;
    uint32_t last_processed_sequence = 0U;

    FSP_PARAMETER_NOT_USED(pvParameters);

    err = RM_ETHOSU_Open(&g_rm_ethosu0_ctrl,
                         &g_rm_ethosu0_cfg);
    if(FSP_SUCCESS != err)
    {
        g_printf("[AI][ERR] RM_ETHOSU_Open failed: %u\r\n",
                 (unsigned int) err);

        ai_thread_fatal_stop(err);
    }

    (void) xEventGroupSetBits(g_ai_app_event,
                              HARDWARE_ETHOSU_INIT_DONE);

    p_model_input = GetModelInputPtr_x();
    p_model_output = GetModelOutputPtr_Identity_70374();

    if((NULL == p_model_input) ||
       (NULL == p_model_output))
    {
        g_printf("[AI][ERR] Model tensor pointer is NULL.\r\n");
        ai_thread_fatal_stop(FSP_ERR_INTERNAL);
    }

    (void) xEventGroupSetBits(g_ai_app_event,
                              SOFTWARE_AI_INFERENCE_INIT_DONE);

    events = xEventGroupWaitBits(g_ai_app_event,
                                 HARDWARE_CAMERA_INIT_DONE,
                                 pdFALSE,
                                 pdTRUE,
                                 portMAX_DELAY);

    if(0U == (events & HARDWARE_CAMERA_INIT_DONE))
    {
        g_printf("[AI][ERR] Camera initialization wait failed.\r\n");
        ai_thread_fatal_stop(FSP_ERR_INTERNAL);
    }

    for(;;)
    {
        uint8_t * p_completed_frame;
        uint32_t completed_sequence = 0U;

        ai_preprocess_status_t preprocess_status;

        yolo_detection_t detections[YOLO_MAX_DETECTIONS];
        int detection_count;

        events = xEventGroupWaitBits(
            g_ai_app_event,
            AI_INFERENCE_INPUT_IMAGE_READY,
            pdTRUE,
            pdFALSE,
            portMAX_DELAY);

        if(0U == (events & AI_INFERENCE_INPUT_IMAGE_READY))
        {
            continue;
        }

        p_completed_frame =
            camera_completed_frame_get(&completed_sequence);

        if(NULL == p_completed_frame)
        {
            continue;
        }

        if(completed_sequence == last_processed_sequence)
        {
            continue;
        }

#if BSP_CFG_DCACHE_ENABLED

        SCB_InvalidateDCache_by_Addr(
            (uint32_t *) p_completed_frame,
            (int32_t) VIN_BYTES_PER_FRAME);

#endif

        preprocess_status =
            ai_preprocess_rgb565_to_int8(
                p_completed_frame,
                VIN_BYTES_PER_FRAME,
                p_model_input,
                AI_PREPROCESS_DESTINATION_BYTES,
                g_ai_preprocess_horizontal_map,
                AI_PREPROCESS_HORIZONTAL_MAP_LENGTH);

        if(AI_PREPROCESS_SUCCESS != preprocess_status)
        {
            g_printf("[AI][ERR] Preprocess failed: status=%u, frame=%u.\r\n",
                     (unsigned int) preprocess_status,
                     (unsigned int) completed_sequence);

            ai_thread_fatal_stop(FSP_ERR_INTERNAL);
        }

        p_completed_frame = NULL;

        RunModel(false);

        detection_count =
            yolo_decode_int8_output(
                p_model_output,
                detections,
                YOLO_MAX_DETECTIONS,
                AI_CONFIDENCE_THRESHOLD);

        detection_count =
            yolo_nms(detections,
                     detection_count,
                     AI_NMS_IOU_THRESHOLD);

        bool publish_ok = ai_inference_result_publish(completed_sequence, detections, detection_count);

        if(!publish_ok)
        {
            APP_ERROR_TRAP(FSP_ERR_INTERNAL);
        }

        __DMB();

        (void)xEventGroupSetBits(g_ai_app_event, AI_INFERENCE_RESULT_UPDATED);
        last_processed_sequence = completed_sequence;

    }
}
