/* generated vector source file - do not edit */
#include "bsp_api.h"
/* Do not build these data structures if no interrupts are currently allocated because IAR will have build errors. */
#if VECTOR_DATA_IRQ_COUNT > 0
        BSP_DONT_REMOVE const fsp_vector_t g_vector_table[BSP_ICU_VECTOR_NUM_ENTRIES] BSP_PLACE_IN_SECTION(BSP_SECTION_APPLICATION_VECTORS) =
        {
                        [0] = iic_master_rxi_isr, /* IIC0 RXI (Receive data full) */
            [1] = iic_master_txi_isr, /* IIC0 TXI (Transmit data empty) */
            [2] = iic_master_tei_isr, /* IIC0 TEI (Transmit end) */
            [3] = iic_master_eri_isr, /* IIC0 ERI (Transfer error) */
            [4] = spi_b_rxi_isr, /* SPI0 RXI (Receive buffer full) */
            [5] = spi_b_txi_isr, /* SPI0 TXI (Transmit buffer empty) */
            [6] = spi_b_tei_isr, /* SPI0 TEI (Transmission complete event) */
            [7] = spi_b_eri_isr, /* SPI0 ERI (Error) */
            [8] = r_icu_isr, /* ICU IRQ0 (External pin interrupt 0) */
            [9] = sci_b_uart_rxi_isr, /* SCI0 RXI (Receive data full) */
            [10] = sci_b_uart_txi_isr, /* SCI0 TXI (Transmit data empty) */
            [11] = sci_b_uart_tei_isr, /* SCI0 TEI (Transmit end) */
            [12] = sci_b_uart_eri_isr, /* SCI0 ERI (Receive error) */
        };
        #if BSP_FEATURE_ICU_HAS_IELSR
        const bsp_interrupt_event_t g_interrupt_event_link_select[BSP_ICU_VECTOR_NUM_ENTRIES] =
        {
            [0] = BSP_PRV_VECT_ENUM(EVENT_IIC0_RXI,GROUP0), /* IIC0 RXI (Receive data full) */
            [1] = BSP_PRV_VECT_ENUM(EVENT_IIC0_TXI,GROUP1), /* IIC0 TXI (Transmit data empty) */
            [2] = BSP_PRV_VECT_ENUM(EVENT_IIC0_TEI,GROUP2), /* IIC0 TEI (Transmit end) */
            [3] = BSP_PRV_VECT_ENUM(EVENT_IIC0_ERI,GROUP3), /* IIC0 ERI (Transfer error) */
            [4] = BSP_PRV_VECT_ENUM(EVENT_SPI0_RXI,GROUP4), /* SPI0 RXI (Receive buffer full) */
            [5] = BSP_PRV_VECT_ENUM(EVENT_SPI0_TXI,GROUP5), /* SPI0 TXI (Transmit buffer empty) */
            [6] = BSP_PRV_VECT_ENUM(EVENT_SPI0_TEI,GROUP6), /* SPI0 TEI (Transmission complete event) */
            [7] = BSP_PRV_VECT_ENUM(EVENT_SPI0_ERI,GROUP7), /* SPI0 ERI (Error) */
            [8] = BSP_PRV_VECT_ENUM(EVENT_ICU_IRQ0,GROUP0), /* ICU IRQ0 (External pin interrupt 0) */
            [9] = BSP_PRV_VECT_ENUM(EVENT_SCI0_RXI,GROUP1), /* SCI0 RXI (Receive data full) */
            [10] = BSP_PRV_VECT_ENUM(EVENT_SCI0_TXI,GROUP2), /* SCI0 TXI (Transmit data empty) */
            [11] = BSP_PRV_VECT_ENUM(EVENT_SCI0_TEI,GROUP3), /* SCI0 TEI (Transmit end) */
            [12] = BSP_PRV_VECT_ENUM(EVENT_SCI0_ERI,GROUP4), /* SCI0 ERI (Receive error) */
        };
        #endif
        #endif
