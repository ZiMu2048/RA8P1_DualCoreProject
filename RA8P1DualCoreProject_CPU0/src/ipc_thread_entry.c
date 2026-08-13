#include "ipc_thread.h"
#include "SEGGER_RTT/bsp_print.h"

/* IPC Thread entry function */
/* pvParameters contains TaskHandle_t */
void ipc_thread_entry(void *pvParameters) {
	FSP_PARAMETER_NOT_USED(pvParameters);

	/*
	 * RA8P1 复位后只有主核 CPU0 自动运行，CPU1 仍处于 power-gating/wait 状态。
	 * 该用户入口执行前，生成代码已经调用 rtos_startup_common_init()，因此系统
	 * 时钟、共享 FSP 资源和 CPU0 HAL 均已初始化。此处再设置 CPU1 向量表并
	 * 释放 CPU1，既满足启动顺序，也不会被 Generate Project Content 覆盖。
	 */
	R_BSP_SecondaryCoreStart();
	g_printf("[CPU0][IPC] CPU1 start requested\r\n");

	/* TODO: 在此继续实现 CPU0 与 CPU1 的 IPC 握手和图像描述符传输。 */
	while (1) {
		vTaskDelay(1);
	}
}
