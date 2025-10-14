#ifndef _PARAMETERS_H_
#define _PARAMETERS_H_

#include "app_config.h"
#include "xparameters.h"

#define UART_BAUDRATE                           115200U
#define SPI_DEVICE_ID				XPAR_SPI_0_DEVICE_ID
#define GPIO_DEVICE_ID				XPAR_GPIO_0_DEVICE_ID
#define UART_DEVICE_ID				XPAR_AXI_UART_DEVICE_ID
#define UART_IRQ_ID                 XPAR_AXI_INTC_AXI_UART_INTERRUPT_INTR
#define INTC_DEVICE_ID				XPAR_SCUGIC_SINGLE_DEVICE_ID

#define GPIO_OFFSET				0 


#define DAC_DDR_BASEADDR			(XPAR_AXI_DDR_CNTRL_BASEADDR + 0x800000) // condition for microblaze

#define TX_CORE_BASEADDR			XPAR_AXI_AD9144_TPL_DAC_TPL_CORE_BASEADDR
#define TX_CORE_HIGHADDR            XPAR_AXI_AD9144_TPL_DAC_TPL_CORE_HIGHADDR

#define TX_DMA_BASEADDR				XPAR_AXI_AD9144_DMA_BASEADDR
#define TX_DMA_HIGHADDR             XPAR_AXI_AD9144_DMA_HIGHADDR

#define TX_JESD_BASEADDR			XPAR_AXI_AD9144_JESD_TX_AXI_BASEADDR
#define TX_JESD_HIGHADDR            XPAR_AXI_AD9144_JESD_TX_AXI_HIGHADDR

#define TX_XCVR_BASEADDR			XPAR_AXI_AD9144_XCVR_BASEADDR
#define TX_XCVR_HIGHADDR            XPAR_AXI_AD9144_XCVR_HIGHADDR

#define TX_PLL_BASEADDR				AD9144_JESD204_LANE_PLL_RECONFIG_BASE



#define TX_ADXCFG_0_BASEADDR			AVL_ADXCFG_0_RCFG_S0_BASE
#define TX_ADXCFG_1_BASEADDR			AVL_ADXCFG_1_RCFG_S0_BASE
#define TX_ADXCFG_2_BASEADDR			AVL_ADXCFG_2_RCFG_S0_BASE
#define TX_ADXCFG_3_BASEADDR			AVL_ADXCFG_3_RCFG_S0_BASE


#define SPI_BASEADDR				XPAR_AXI_SPI_BASEADDR
#define SPI_HIGHADDR                XPAR_AXI_SPI_HIGHADDR
#define GPIO_BASEADDR				XPAR_GPIO_0_BASEADDR
#define GPIO_HIGHADDR               XPAR_GPIO_0_HIGHADDR

// FMC pins for  GPIO connections to SPI, these correspond to the verilog code 

// add the corresponding SPI links if required to debug. 

#define GPIO_CLKD_SYNC			(GPIO_OFFSET + 20)
#define GPIO_DAC_RESET			(GPIO_OFFSET + 19)
#define GPIO_DAC_TXEN			(GPIO_OFFSET + 18)
#define GPIO_DAC_CTRL_1			(GPIO_OFFSET + 22)
#define GPIO_DAC_CTRL_0			(GPIO_OFFSET + 21)

// all clocks are differential, and only 4 outputs from the clocks are present. 
enum ad9516_channels {
    DAC_DEVICE_CLK,      // Output 1: External clock provided
    DAC_DEVICE_SYSREF,   // Output 6: System ref for the DAC
    DAC_FPGA_CLK,        // Output 9: Clock from the FPGA
    DAC_FPGA_SYSREF      // Output 7: System reference clock for the FPGA
};
#endif