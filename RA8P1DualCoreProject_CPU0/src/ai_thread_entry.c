#include "ai_thread.h"
#include "common/common.h"
#include "Camera/camera_capture.h"
#include "AI/ai_preprocess.h"
#include "AI/yolo_postprocess.h"
#include "model/model.h"
#include "SEGGER_RTT/bsp_print.h"

#include <stddef.h>
#include <stdint.h>

#define AI_CONFIDENCE_THRESHOLD       (0.50f)
#define AI_NMS_IOU_THRESHOLD          (0.45f)

/*正式版删除*/
#define AI_RTT_REPORT_INTERVAL        (30U)

/*
 *[@name] g_ai_preprocess_horizontal_map
 *[@type] static global variable
 *[@usage] 保存Helium预处理使用的横向源像素索引，只允许AI Thread访问
 */
static uint16_t
    g_ai_preprocess_horizontal_map[AI_PREPROCESS_HORIZONTAL_MAP_LENGTH]
    BSP_ALIGN_VARIABLE(16);

/*
 *[@name] g_ai_inference_count
 *[@type] static volatile global variable
 *[@usage] 记录AI Thread成功完成预处理、推理和后处理的累计次数，供调试器观察
 */
static volatile uint32_t g_ai_inference_count;

/*
 *[@name] g_ai_last_processed_sequence
 *[@type] static volatile global variable
 *[@usage] 记录AI Thread最近一次成功处理的VIN帧序号，避免重复处理同一帧
 */
static volatile uint32_t g_ai_last_processed_sequence;

/*
 *[@name] g_ai_last_detection_count
 *[@type] static volatile global variable
 *[@usage] 记录最近一次YOLO后处理保留的检测框数量，当前仅供调试和RTT观察
 */
static volatile uint32_t g_ai_last_detection_count;

/*
 *[@name] g_ai_last_confidence_milli
 *[@type] static volatile global variable
 *[@usage] 记录最近一次推理的最大置信度千分值，例如875表示0.875
 */
static volatile uint32_t g_ai_last_confidence_milli;

/*
 *[@name] g_ai_last_preprocess_time_ms
 *[@type] static volatile global variable
 *[@usage] 记录最近一次Helium预处理的任务墙钟时间，单位为毫秒
 */
static volatile uint32_t g_ai_last_preprocess_time_ms;

/*
 *[@name] g_ai_last_model_time_ms
 *[@type] static volatile global variable
 *[@usage] 记录最近一次RunModel总执行时间，包含NPU、CPU回退和任务抢占时间，单位为毫秒
 */
static volatile uint32_t g_ai_last_model_time_ms;

/*
 *[@name] g_ai_last_postprocess_time_ms
 *[@type] static volatile global variable
 *[@usage] 记录最近一次YOLO解码与NMS总时间，单位为毫秒
 */
static volatile uint32_t g_ai_last_postprocess_time_ms;

/*
 *[@name] g_ai_null_frame_count
 *[@type] static volatile global variable
 *[@usage] 记录AI Thread收到输入事件但未取得有效完成帧的累计次数
 */
static volatile uint32_t g_ai_null_frame_count;

/*
 *[@name] g_ai_duplicate_frame_count
 *[@type] static volatile global variable
 *[@usage] 记录AI Thread跳过重复帧序号的累计次数
 */
static volatile uint32_t g_ai_duplicate_frame_count;

/*
 *[@name] g_ai_preprocess_error_count
 *[@type] static volatile global variable
 *[@usage] 记录Helium预处理失败的累计次数，正常运行时应保持为0
 */
static volatile uint32_t g_ai_preprocess_error_count;

/*
 *[@name] ai_thread_fatal_stop
 *[@type] static function
 *[@usage] 打印AI致命错误并触发CPU0断点，调试器继续执行后使AI Thread保持阻塞
 *[@argument] error 需要记录和触发断点的FSP错误码
 *[@return] none，此函数不会返回
 */
static void ai_thread_fatal_stop(fsp_err_t error)
{
    g_printf("[AI][ERR] Fatal error: %u\r\n",
             (unsigned int) error);

    APP_ERROR_TRAP(error);

    for(;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

/*
 *[@name] ai_thread_ticks_to_ms
 *[@type] static function
 *[@usage] 将FreeRTOS Tick差值转换为毫秒，用于第一阶段粗略测量各处理阶段墙钟时间
 *[@argument] ticks 需要转换的FreeRTOS Tick数量
 *[@return] 返回对应的毫秒数
 */
static uint32_t ai_thread_ticks_to_ms(TickType_t ticks)
{
    return ((uint32_t) ticks * (uint32_t) portTICK_PERIOD_MS);
}

/*
 *[@name] ai_thread_max_confidence_milli_get
 *[@type] static function
 *[@usage] 遍历YOLO检测结果并把最大浮点置信度转换为0到1000的整数，避免RTT打印浮点数
 *[@argument] p_detections YOLO检测结果数组首地址
 *[@argument] detection_count 检测结果有效元素数量
 *[@return] 返回最大置信度千分值，无有效检测结果时返回0
 */
static uint32_t ai_thread_max_confidence_milli_get(
    yolo_detection_t const * p_detections,
    int detection_count)
{
    float max_confidence = 0.0f;

    if((NULL == p_detections) || (detection_count <= 0))
    {
        return 0U;
    }

    for(int index = 0; index < detection_count; index++)
    {
        if(p_detections[index].score > max_confidence)
        {
            max_confidence = p_detections[index].score;
        }
    }

    if(max_confidence >= 1.0f)
    {
        return 1000U;
    }

    if(max_confidence <= 0.0f)
    {
        return 0U;
    }

    return (uint32_t) (max_confidence * 1000.0f + 0.5f);
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

    FSP_PARAMETER_NOT_USED(pvParameters);

    g_ai_inference_count = 0U;
    g_ai_last_processed_sequence = 0U;
    g_ai_last_detection_count = 0U;
    g_ai_last_confidence_milli = 0U;
    g_ai_last_preprocess_time_ms = 0U;
    g_ai_last_model_time_ms = 0U;
    g_ai_last_postprocess_time_ms = 0U;
    g_ai_null_frame_count = 0U;
    g_ai_duplicate_frame_count = 0U;
    g_ai_preprocess_error_count = 0U;

    g_printf("\r\n[AI] AI Thread started.\r\n");
    g_printf("[AI] Opening Ethos-U55 runtime.\r\n");

    /*
     * RM_ETHOSU只能由AI Thread打开一次。
     * 后续Camera和Display Thread均不得调用该实例。
     */
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

    g_printf("[AI] Ethos-U55 runtime opened.\r\n");

    /*
     * 模型输入和输出位于生成模型的静态Arena中。
     * AI Thread不为张量调用malloc，也不释放这些地址。
     */
    p_model_input = GetModelInputPtr_x();
    p_model_output = GetModelOutputPtr_Identity_70374();

    if((NULL == p_model_input) ||
       (NULL == p_model_output))
    {
        g_printf("[AI][ERR] Model tensor pointer is NULL.\r\n");
        ai_thread_fatal_stop(FSP_ERR_INTERNAL);
    }

    g_printf("[AI] Model tensors ready, input=0x%08X, output=0x%08X.\r\n",
             (unsigned int) (uintptr_t) p_model_input,
             (unsigned int) (uintptr_t) p_model_output);

    (void) xEventGroupSetBits(g_ai_app_event,
                              SOFTWARE_AI_INFERENCE_INIT_DONE);

    g_printf("[AI] Waiting for Camera Thread initialization.\r\n");

    /*
     * HARDWARE_CAMERA_INIT_DONE是持续状态位，因此不能清除。
     * 实际图像处理仍要等待独立的AI帧事件。
     */
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

    g_printf("[AI] Camera initialized; waiting for input frames.\r\n");

    for(;;)
    {
        uint8_t * p_completed_frame;
        uint32_t completed_sequence = 0U;

        ai_preprocess_status_t preprocess_status;

        yolo_detection_t detections[YOLO_MAX_DETECTIONS];
        int detection_count;

        TickType_t stage_start_tick;
        TickType_t stage_end_tick;

        /*
         * AI使用自己的事件位并在取走后清除。
         * Display Thread清除CAMERA_FRAME_READY不会影响AI。
         */
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

        /*
         * camera_completed_frame_get内部用FreeRTOS临界区保证
         * 帧地址和帧序号来自同一次快照。
         */
        p_completed_frame =
            camera_completed_frame_get(&completed_sequence);

        if(NULL == p_completed_frame)
        {
            g_ai_null_frame_count++;
            continue;
        }

        if(completed_sequence == g_ai_last_processed_sequence)
        {
            g_ai_duplicate_frame_count++;
            continue;
        }

#if BSP_CFG_DCACHE_ENABLED

        /*
         * VIN通过DMA写SDRAM，CPU读取前必须丢弃D-Cache中的旧副本。
         * AI和Display都只读取VIN缓冲区，因此两边各自Invalidate是安全的。
         */
        SCB_InvalidateDCache_by_Addr(
            (uint32_t *) p_completed_frame,
            (int32_t) VIN_BYTES_PER_FRAME);

#endif

        /*
         * 第一阶段：Helium裁剪、缩放、RGB565转换和INT8量化。
         */
        stage_start_tick = xTaskGetTickCount();

        preprocess_status =
            ai_preprocess_rgb565_to_int8(
                p_completed_frame,
                VIN_BYTES_PER_FRAME,
                p_model_input,
                AI_PREPROCESS_DESTINATION_BYTES,
                g_ai_preprocess_horizontal_map,
                AI_PREPROCESS_HORIZONTAL_MAP_LENGTH);

        stage_end_tick = xTaskGetTickCount();

        g_ai_last_preprocess_time_ms =
            ai_thread_ticks_to_ms(stage_end_tick -
                                  stage_start_tick);

        if(AI_PREPROCESS_SUCCESS != preprocess_status)
        {
            g_ai_preprocess_error_count++;

            g_printf("[AI][ERR] Preprocess failed: status=%u, frame=%u.\r\n",
                     (unsigned int) preprocess_status,
                     (unsigned int) completed_sequence);

            ai_thread_fatal_stop(FSP_ERR_INTERNAL);
        }

        /*
         * 预处理完成后不再访问VIN帧指针。
         * 后续推理只访问模型自己的输入张量。
         */
        p_completed_frame = NULL;

        /*
         * 第二阶段：执行完整模型。
         *
         * 该时间包含Ethos-U55子图、CPU回退算子和被高优先级
         * Camera/Display Thread抢占的时间，不等同于纯NPU时间。
         */
        stage_start_tick = xTaskGetTickCount();

        RunModel(false);

        stage_end_tick = xTaskGetTickCount();

        g_ai_last_model_time_ms =
            ai_thread_ticks_to_ms(stage_end_tick -
                                  stage_start_tick);

        /*
         * 第三阶段：解码[336,5]输出并执行NMS。
         */
        stage_start_tick = xTaskGetTickCount();

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

        stage_end_tick = xTaskGetTickCount();

        g_ai_last_postprocess_time_ms =
            ai_thread_ticks_to_ms(stage_end_tick -
                                  stage_start_tick);

        g_ai_last_detection_count =
            (detection_count > 0) ?
            (uint32_t) detection_count :
            0U;

        g_ai_last_confidence_milli =
            ai_thread_max_confidence_milli_get(
                detections,
                detection_count);

        g_ai_last_processed_sequence = completed_sequence;
        g_ai_inference_count++;

        /*
         * 正式版删除
         *
         * RTT缓冲区只有2048字节，并且使用NO_BLOCK_SKIP模式。
         * 每30次推理打印一次，避免逐帧日志干扰实时处理。
         */
        /*正式版删除*/
        if(0U == (g_ai_inference_count %
                  AI_RTT_REPORT_INTERVAL))
        {
            g_printf(
                "[AI] frame=%u, inference=%u, detections=%u, "
                "confidence=%u/1000, pre=%u ms, model=%u ms, post=%u ms.\r\n",
                (unsigned int) g_ai_last_processed_sequence,
                (unsigned int) g_ai_inference_count,
                (unsigned int) g_ai_last_detection_count,
                (unsigned int) g_ai_last_confidence_milli,
                (unsigned int) g_ai_last_preprocess_time_ms,
                (unsigned int) g_ai_last_model_time_ms,
                (unsigned int) g_ai_last_postprocess_time_ms);
        }

        /*
         * 当前阶段不设置AI_INFERENCE_RESULT_UPDATED。
         *
         * 原因是检测结果还没有进入正式的结果快照模块。
         * 等D/AVE 2D阶段建立ai_inference_result.c/.h后，
         * 再由AI Thread发布结果，由Display Thread读取并画框。
         */
    }
}