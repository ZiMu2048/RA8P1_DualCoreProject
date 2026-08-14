/* generated configuration header file - do not edit */
#ifndef BSP_PIN_CFG_H_
#define BSP_PIN_CFG_H_
#include "r_ioport.h"

/* Common macro for FSP header files. There is also a corresponding FSP_FOOTER macro at the end of this file. */
FSP_HEADER

#define NRF_VIDEO_MISO (BSP_IO_PORT_01_PIN_00)
#define NRF_VIDEO_MOSI (BSP_IO_PORT_01_PIN_01)
#define NRF_VIDEO_SCK (BSP_IO_PORT_01_PIN_02)
#define NRF_VIDEO_CSN (BSP_IO_PORT_01_PIN_03)
#define NRF_VIDEO_CE (BSP_IO_PORT_01_PIN_04)
#define NRF_COMMAND_IRQ (BSP_IO_PORT_01_PIN_05)
#define NRF_COMMAND_MISO (BSP_IO_PORT_07_PIN_00)
#define NRF_COMMAND_MOSI (BSP_IO_PORT_07_PIN_01)
#define NRF_COMMAND_SCK (BSP_IO_PORT_07_PIN_02)
#define NRF_COMMAND_CSN (BSP_IO_PORT_07_PIN_03)
#define NRF_COMMAND_CE (BSP_IO_PORT_07_PIN_04)

extern const ioport_cfg_t g_bsp_pin_cfg; /* R7KA8P1KFLCAC.pincfg */

void BSP_PinConfigSecurityInit();

/* Common macro for FSP header files. There is also a corresponding FSP_HEADER macro at the top of this file. */
FSP_FOOTER
#endif /* BSP_PIN_CFG_H_ */
