#include "nRF27L01.h"

/*
 *[@name] nRF27L01_entry
 *[@type] thread entry function
 *[@usage] 保留nRF24L01相关线程入口，当前M85不操作该无线模块
 *[@argument] pvParameters FSP传入的线程参数，当前未使用
 *[@return] none
 */
void nRF27L01_entry(void *pvParameters) {
	FSP_PARAMETER_NOT_USED(pvParameters);

	/* TODO: add your own code here */
	while (1) {
		vTaskDelay(1);
	}
}
