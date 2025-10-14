
#ifndef _PARAMETERS_H_
#define _PARAMETERS_H_

#include "app_config.h"

#include "xparameters.h"


#define UART_BAUDRATE                           115200

#define SPI_DEVICE_ID				XPAR_SPI_0_DEVICE_ID
#define GPIO_DEVICE_ID				XPAR_GPIO_0_DEVICE_ID
#define UART_DEVICE_ID				XPAR_AXI_UART_DEVICE_ID
#define INTC_DEVICE_ID				XPAR_SCUGIC_SINGLE_DEVICE_ID

#define GPIO_OFFSET				78 // either it is 78 or 54 or 0
#define DAC_DDR_BASEADDR			(XPAR_AXI_DDR_CNTRL_BASEADDR + 0x900000) // condition for microblaze
#define TX_CORE_BASEADDR			XPAR_AXI_AD9144_TPL_DAC_TPL_CORE_BASEADDR // or this AXI_AD9144_CORE_BASE + 0x4000
#define TX_DMA_BASEADDR				XPAR_AXI_AD9144_DMA_BASEADDR
#define TX_JESD_BASEADDR			XPAR_AXI_AD9144_JESD_TX_AXI_BASEADDR
#define TX_XCVR_BASEADDR			XPAR_AXI_AD9144_XCVR_BASEADDR
#define SPI_BASEADDR				SYS_SPI_BASE
#define GPIO_BASEADDR				SYS_GPIO_OUT_BASE
#define TX_ADXCFG_0_BASEADDR			AVL_ADXCFG_0_RCFG_S0_BASE
#define TX_ADXCFG_1_BASEADDR			AVL_ADXCFG_1_RCFG_S0_BASE
#define TX_ADXCFG_2_BASEADDR			AVL_ADXCFG_2_RCFG_S0_BASE
#define TX_ADXCFG_3_BASEADDR			AVL_ADXCFG_3_RCFG_S0_BASE

#define GPIO_TRIG				(GPIO_OFFSET + 43)
#define GPIO_DAC_TXEN				(GPIO_OFFSET + 41)
#define GPIO_DAC_RESET				(GPIO_OFFSET + 40)
#define GPIO_CLKD_SYNC				(GPIO_OFFSET + 38)
#define GPIO_SPI_EN                 (GPIO_OFFSET + 35)
#define GPIO_DAC_IRQ				(GPIO_OFFSET + 34)
#define GPIO_CLKD_STATUS_1			(GPIO_OFFSET + 33)
#define GPIO_CLKD_STATUS_0			(GPIO_OFFSET + 32) // cross check these values and see how these are being made
enum ad9516_fpga_channels {
	//DAC_DEVICE_CLK,  this is the default clock input that I am providing to the evaluation board. 
	DAC_FPGA_CLK,
	DAC_FPGA_SYSREF
};

enum  ad9516_dac_channels {
	DAC_DEVICE_SYSREF
};
#endif /* _PARAMETERS_H_ */

