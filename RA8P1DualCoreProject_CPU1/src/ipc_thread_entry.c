#include "ipc_thread.h"
#include "Radio/adapters/rtos/video_frame_mailbox.h"

/* IPC Thread entry function */
/* pvParameters contains TaskHandle_t */
void ipc_thread_entry(void *pvParameters) {
	FSP_PARAMETER_NOT_USED(pvParameters);

	/*
	 * TODO：收到M85“共享内存图像就绪”事件后：
	 * 1. 校验共享内存描述符（地址范围、长度、帧号、CRC）；
	 * 2. 对共享区域执行M33所需的Cache失效操作；
	 * 3. 构造 video_frame_t，并调用 VideoFrameMailbox_Publish()；
	 * 4. VideoFrameMailbox_CompletionTake()返回完成后，再通过IPC通知M85复用缓冲区。
	 *
	 * 不在这里直接调用NRF驱动：SPI1始终只归Video TX Thread所有。
	 */
	while (1) {
		vTaskDelay(1);
	}
}
