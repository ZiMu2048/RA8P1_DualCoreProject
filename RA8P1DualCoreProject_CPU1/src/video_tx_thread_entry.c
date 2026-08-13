#include "video_tx_thread.h"
#include "Radio/adapters/rtos/video_frame_mailbox.h"
#include "Radio/application/video_radio.h"
#include "SEGGER_RTT/bsp_print.h"

/* Video TX Thread entry function */
/* pvParameters contains TaskHandle_t */
void video_tx_thread_entry(void *pvParameters) {
	FSP_PARAMETER_NOT_USED(pvParameters);

	nrf24_result_t result = VideoRadio_Init();
	if (NRF24_RESULT_SUCCESS != result) {
		g_printf("[VIDEO NRF] init failed=%u\r\n", (uint32_t) result);
		vTaskSuspend(NULL);
	}

	g_printf("[VIDEO NRF] ready SPI1 ch=100\r\n");
	while (1) {
		video_frame_t frame;
		if (!VideoFrameMailbox_Acquire(&frame)) {
			vTaskDelay(pdMS_TO_TICKS(1U));
			continue;
		}

		result = VideoRadio_SendFrame(&frame);
		VideoFrameMailbox_Complete(frame.frame_id,
		                           NRF24_RESULT_SUCCESS == result);
		if (NRF24_RESULT_SUCCESS != result) {
			g_printf("[VIDEO NRF] frame=%u send failed=%u\r\n",
			          (uint32_t) frame.frame_id,
			          (uint32_t) result);
		}
	}
}
