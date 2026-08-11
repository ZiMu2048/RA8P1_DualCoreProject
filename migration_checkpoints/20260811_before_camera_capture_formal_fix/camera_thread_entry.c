#include "camera_thread.h"
#include "Camera/i2c_control.h"
#include "Camera/camera_sensor.h"
#include "common/common.h"
#include "SEGGER_RTT/bsp_print.h"

static volatile fsp_err_t g_camera_init_error = FSP_SUCCESS;

/* Camera Thread entry function */
/* pvParameters contains TaskHandle_t */
void camera_thread_entry(void * pvParameters)
{
    FSP_PARAMETER_NOT_USED(pvParameters);

    fsp_err_t err = FSP_SUCCESS;

    uint8_t GLCDC_Buffer_index =1U;

    err = i2c_control_init();

    vTaskDelay(pdMS_TO_TICKS(10U));

    err = camera_open();

    vTaskDelay(pdMS_TO_TICKS(10U));

    err = glcdc_init();

    memset(vin_image_buffer_1, RESET_VALUE, VIN_BYTES_PER_FRAME);
    memset(vin_image_buffer_2, RESET_VALUE, VIN_BYTES_PER_FRAME);
    memset(vin_image_buffer_3, RESET_VALUE, VIN_BYTES_PER_FRAME);

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

    //err = vin_camera_start(&g_vin_cfg_run_time);

    vTaskDelay(pdMS_TO_TICKS(10U));

    err = camera_write_array(&live_camera);

    vTaskDelay(pdMS_TO_TICKS(10U));

    while(true)
    {
        

    }







}
