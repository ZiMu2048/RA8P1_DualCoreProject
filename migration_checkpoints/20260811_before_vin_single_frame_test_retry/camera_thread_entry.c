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

    vTaskDelay(pdMS_TO_TICKS(10U));

    g_printf("[CAM] IIC master opened.\r\n");
    g_printf("[CAM] Initializing OV5640...\r\n");
    g_camera_init_error = camera_open();
    if (FSP_SUCCESS != g_camera_init_error)
    {
        g_printf("[CAM][ERR] OV5640 initialization failed: %u\r\n", (unsigned int) g_camera_init_error);
        APP_ERROR_TRAP(g_camera_init_error);//<-卡这里了
        while (true)
        {
            vTaskDelay(pdMS_TO_TICKS(1000U));
        }
    }

    xEventGroupSetBits(g_ai_app_event, HARDWARE_CAMERA_INIT_DONE);
    g_printf("[CAM] OV5640 initialization completed.\r\n");
    g_printf("[CAM] HARDWARE_CAMERA_INIT_DONE set.\r\n");

    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}
