#include "encode_thread.h"

/*
 *[@name] encode_thread_entry
 *[@type] thread entry function
 *[@usage] 保留黑白JPEG编码线程入口，当前阶段只等待后续图传作业
 *[@argument] pvParameters FSP传入的线程参数，当前未使用
 *[@return] none
 */
void encode_thread_entry(void *pvParameters) {
	FSP_PARAMETER_NOT_USED(pvParameters);

	/* TODO: add your own code here */
	while (1) {
		vTaskDelay(1);
	}
}
