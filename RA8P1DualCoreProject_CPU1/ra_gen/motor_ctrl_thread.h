/* generated thread header file - do not edit */
#ifndef MOTOR_CTRL_THREAD_H_
#define MOTOR_CTRL_THREAD_H_
#include "bsp_api.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "hal_data.h"
#ifdef __cplusplus
                extern "C" void motor_ctrl_thread_entry(void * pvParameters);
                #else
extern void motor_ctrl_thread_entry(void *pvParameters);
#endif
FSP_HEADER
FSP_FOOTER
#endif /* MOTOR_CTRL_THREAD_H_ */
