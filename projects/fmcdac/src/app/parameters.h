#ifndef _PARAMETERS_H_
#define _PARAMETERS_H_

#include "app_config.h"
#include "xparameters.h"

#define UART_BAUDRATE                           115200U
#define SPI_DEVICE_ID				XPAR_SPI_0_DEVICE_ID
#define GPIO_DEVICE_ID				XPAR_GPIO_0_DEVICE_ID
#define UART_DEVICE_ID				XPAR_AXI_UART_DEVICE_ID
#define UART_IRQ_ID                 XPAR_AXI_INTC_AXI_UART_INTERRUPT_INTR

/* FMCDAC targets MicroBlaze.  Keep the PS alias only for source portability. */
#if defined(XPAR_INTC_0_DEVICE_ID)
#define INTC_DEVICE_ID                         XPAR_INTC_0_DEVICE_ID
#elif defined(XPAR_AXI_INTC_0_DEVICE_ID)
#define INTC_DEVICE_ID                         XPAR_AXI_INTC_0_DEVICE_ID
#elif defined(XPAR_AXI_INTC_DEVICE_ID)
#define INTC_DEVICE_ID                         XPAR_AXI_INTC_DEVICE_ID
#elif defined(XPAR_SCUGIC_SINGLE_DEVICE_ID)
#define INTC_DEVICE_ID                         XPAR_SCUGIC_SINGLE_DEVICE_ID
#else
#error "Interrupt controller device ID not defined in xparameters.h"
#endif

#define GPIO_OFFSET				0 


#define DAC_DDR_BASEADDR			(XPAR_AXI_DDR_CNTRL_BASEADDR + 0x800000) // condition for microblaze

#ifndef AWG_STREAM_DDR_BASEADDR
#define AWG_STREAM_DDR_BASEADDR			(XPAR_AXI_DDR_CNTRL_BASEADDR + 0x01000000u)
#endif

#ifndef AWG_STREAM_DDR_SIZE_BYTES
#define AWG_STREAM_DDR_SIZE_BYTES		0x00100000u
#endif

#define AWG_DMA_CACHELINE_BYTES                 64U
#define AWG_ETH_DMA_BUFFER_BYTES                2048U
#define AWG_ETH_DMA_BUFFER_COUNT                2U

#ifndef AWG_ETH_RX_DDR_BASEADDR
#define AWG_ETH_RX_DDR_BASEADDR \
	(AWG_STREAM_DDR_BASEADDR + AWG_STREAM_DDR_SIZE_BYTES)
#endif

#ifndef AWG_ETH_TX_DDR_BASEADDR
#define AWG_ETH_TX_DDR_BASEADDR \
	(AWG_ETH_RX_DDR_BASEADDR + \
	 (AWG_ETH_DMA_BUFFER_BYTES * AWG_ETH_DMA_BUFFER_COUNT))
#endif

/* Phase E block-design ABI.  Generated XPAR values remain authoritative. */
#define AWG_PHASE_E_SCHED_BASEADDR              0x44AA0000U
#define AWG_PHASE_E_SCHED_DMA_BASEADDR          0x44AB0000U
#define AWG_PHASE_E_ETH_MAC_BASEADDR            0x44C00000U
#define AWG_PHASE_E_ETH_RX_DMA_BASEADDR         0x44AC0000U
#define AWG_PHASE_E_ETH_TX_DMA_BASEADDR         0x44AD0000U

#if FMCDAC_AWG_SCHED
#ifndef XPAR_AWG_TIMED_CTRL_0_BASEADDR
#error "Phase F scheduler requires XPAR_AWG_TIMED_CTRL_0_BASEADDR from the licensed XSA"
#endif
#if XPAR_AWG_TIMED_CTRL_0_BASEADDR != AWG_PHASE_E_SCHED_BASEADDR
#error "Scheduler XPAR base does not match the Phase E HDL ABI"
#endif
#if defined(FMCDAC_AWG_SCHED_BASEADDR) && \
    FMCDAC_AWG_SCHED_BASEADDR != XPAR_AWG_TIMED_CTRL_0_BASEADDR
#error "FMCDAC scheduler base override disagrees with generated XPAR"
#endif
#define AWG_SCHED_BASEADDR                      XPAR_AWG_TIMED_CTRL_0_BASEADDR
#endif

#if FMCDAC_AWG_SCHED_DMA_REFILL
#ifndef XPAR_AXI_SCHED_DMA_BASEADDR
#error "Scheduler DMA XPAR base is missing from the licensed XSA"
#endif
#if XPAR_AXI_SCHED_DMA_BASEADDR != AWG_PHASE_E_SCHED_DMA_BASEADDR
#error "Scheduler DMA XPAR base does not match the Phase E HDL ABI"
#endif
#if defined(FMCDAC_AWG_SCHED_DMA_BASEADDR) && \
    FMCDAC_AWG_SCHED_DMA_BASEADDR != XPAR_AXI_SCHED_DMA_BASEADDR
#error "FMCDAC scheduler DMA base override disagrees with generated XPAR"
#endif
#define AWG_SCHED_DMA_BASEADDR                  XPAR_AXI_SCHED_DMA_BASEADDR
#endif

#if FMCDAC_AWG_SCHED_ETH
#if !defined(XPAR_ETH_MAC_10G_BASEADDR) || \
    !defined(XPAR_AXI_ETH_RX_DMA_BASEADDR) || \
    !defined(XPAR_AXI_ETH_TX_DMA_BASEADDR)
#error "Phase F Ethernet XPAR bases are missing from the licensed XSA"
#endif
#if XPAR_ETH_MAC_10G_BASEADDR != AWG_PHASE_E_ETH_MAC_BASEADDR || \
    XPAR_AXI_ETH_RX_DMA_BASEADDR != AWG_PHASE_E_ETH_RX_DMA_BASEADDR || \
    XPAR_AXI_ETH_TX_DMA_BASEADDR != AWG_PHASE_E_ETH_TX_DMA_BASEADDR
#error "Ethernet XPAR base does not match the Phase E HDL ABI"
#endif
#if defined(FMCDAC_AWG_ETH_MAC_BASEADDR) && \
    FMCDAC_AWG_ETH_MAC_BASEADDR != XPAR_ETH_MAC_10G_BASEADDR
#error "FMCDAC MAC base override disagrees with generated XPAR"
#endif
#if defined(FMCDAC_AWG_ETH_RX_DMA_BASEADDR) && \
    FMCDAC_AWG_ETH_RX_DMA_BASEADDR != XPAR_AXI_ETH_RX_DMA_BASEADDR
#error "FMCDAC RX DMA base override disagrees with generated XPAR"
#endif
#if defined(FMCDAC_AWG_ETH_TX_DMA_BASEADDR) && \
    FMCDAC_AWG_ETH_TX_DMA_BASEADDR != XPAR_AXI_ETH_TX_DMA_BASEADDR
#error "FMCDAC TX DMA base override disagrees with generated XPAR"
#endif
#define AWG_ETH_MAC_BASEADDR                    XPAR_ETH_MAC_10G_BASEADDR
#define AWG_ETH_RX_DMA_BASEADDR                 XPAR_AXI_ETH_RX_DMA_BASEADDR
#define AWG_ETH_TX_DMA_BASEADDR                 XPAR_AXI_ETH_TX_DMA_BASEADDR
#endif

#if FMCDAC_AWG_SCHED_USE_IRQ
#if defined(XPAR_AXI_INTC_AWG_TIMED_CTRL_0_IRQ_INTR)
#define AWG_SCHED_IRQ_ID XPAR_AXI_INTC_AWG_TIMED_CTRL_0_IRQ_INTR
#elif defined(XPAR_INTC_0_AWG_TIMED_CTRL_0_IRQ_INTR)
#define AWG_SCHED_IRQ_ID XPAR_INTC_0_AWG_TIMED_CTRL_0_IRQ_INTR
#else
#error "Scheduler IRQ ID is missing from generated xparameters.h"
#endif
#endif

#if FMCDAC_AWG_SCHED_DMA_REFILL
#if defined(XPAR_AXI_INTC_AXI_SCHED_DMA_IRQ_INTR)
#define AWG_SCHED_DMA_IRQ_ID XPAR_AXI_INTC_AXI_SCHED_DMA_IRQ_INTR
#elif defined(XPAR_INTC_0_AXI_SCHED_DMA_IRQ_INTR)
#define AWG_SCHED_DMA_IRQ_ID XPAR_INTC_0_AXI_SCHED_DMA_IRQ_INTR
#else
#error "Scheduler DMA IRQ ID is missing from generated xparameters.h"
#endif
#endif

#if FMCDAC_AWG_SCHED_ETH
#if defined(XPAR_AXI_INTC_AXI_ETH_RX_DMA_IRQ_INTR)
#define AWG_ETH_RX_DMA_IRQ_ID XPAR_AXI_INTC_AXI_ETH_RX_DMA_IRQ_INTR
#elif defined(XPAR_INTC_0_AXI_ETH_RX_DMA_IRQ_INTR)
#define AWG_ETH_RX_DMA_IRQ_ID XPAR_INTC_0_AXI_ETH_RX_DMA_IRQ_INTR
#else
#error "Ethernet RX DMA IRQ ID is missing from generated xparameters.h"
#endif
#if defined(XPAR_AXI_INTC_AXI_ETH_TX_DMA_IRQ_INTR)
#define AWG_ETH_TX_DMA_IRQ_ID XPAR_AXI_INTC_AXI_ETH_TX_DMA_IRQ_INTR
#elif defined(XPAR_INTC_0_AXI_ETH_TX_DMA_IRQ_INTR)
#define AWG_ETH_TX_DMA_IRQ_ID XPAR_INTC_0_AXI_ETH_TX_DMA_IRQ_INTR
#else
#error "Ethernet TX DMA IRQ ID is missing from generated xparameters.h"
#endif
#endif

#if (AWG_STREAM_DDR_BASEADDR % AWG_DMA_CACHELINE_BYTES) != 0U || \
    (AWG_STREAM_DDR_SIZE_BYTES % 32U) != 0U
#error "AWG scheduler ring must be cacheline aligned and an event-size multiple"
#endif

#if FMCDAC_AWG_SCHED_ETH
#if (AWG_ETH_RX_DDR_BASEADDR % AWG_DMA_CACHELINE_BYTES) != 0U || \
    (AWG_ETH_TX_DDR_BASEADDR % AWG_DMA_CACHELINE_BYTES) != 0U
#error "Ethernet DMA buffers must be cacheline aligned"
#endif
#ifdef XPAR_AXI_DDR_CNTRL_HIGHADDR
#if (AWG_ETH_TX_DDR_BASEADDR + \
     (AWG_ETH_DMA_BUFFER_BYTES * AWG_ETH_DMA_BUFFER_COUNT) - 1U) > \
    XPAR_AXI_DDR_CNTRL_HIGHADDR
#error "Phase F DMA buffers exceed the generated DDR address range"
#endif
#endif
#endif

#define TX_CORE_BASEADDR			XPAR_AXI_AD9144_TPL_DAC_TPL_CORE_BASEADDR
#define TX_CORE_HIGHADDR            XPAR_AXI_AD9144_TPL_DAC_TPL_CORE_HIGHADDR

#define TX_DMA_BASEADDR				XPAR_AXI_AD9144_DMA_BASEADDR
#define TX_DMA_HIGHADDR             XPAR_AXI_AD9144_DMA_HIGHADDR

#define TX_JESD_BASEADDR			XPAR_AXI_AD9144_JESD_TX_AXI_BASEADDR
#define TX_JESD_HIGHADDR            XPAR_AXI_AD9144_JESD_TX_AXI_HIGHADDR

#define TX_XCVR_BASEADDR			XPAR_AXI_AD9144_XCVR_BASEADDR
#define TX_XCVR_HIGHADDR            XPAR_AXI_AD9144_XCVR_HIGHADDR


#define SPI_BASEADDR				XPAR_AXI_SPI_BASEADDR
#define SPI_HIGHADDR                XPAR_AXI_SPI_HIGHADDR
#define GPIO_BASEADDR				XPAR_GPIO_0_BASEADDR
#define GPIO_HIGHADDR               XPAR_GPIO_0_HIGHADDR

// FMC pins for  GPIO connections to SPI, these correspond to the verilog code 

// add the corresponding SPI links if required to debug. 

#define GPIO_DAC_CTRL_1			(GPIO_OFFSET + 22)
#define GPIO_DAC_CTRL_0			(GPIO_OFFSET + 21)

#define GPIO_DAC_TXEN			(GPIO_OFFSET + 41)
#define GPIO_DAC_RESET			(GPIO_OFFSET + 40)
#define GPIO_SFP0_TX_DISABLE		(GPIO_OFFSET + 26)
#define GPIO_CLKD_SYNC			(GPIO_OFFSET + 38)

// all clocks are differential, and only 4 outputs from the clocks are present. 
enum ad9516_channels {
    DAC_DEVICE_CLK,      // Output 1: External clock provided
    DAC_DEVICE_SYSREF,   // Output 6: System ref for the DAC
    DAC_FPGA_CLK,        // Output 9: Clock from the FPGA
    DAC_FPGA_SYSREF      // Output 7: System reference clock for the FPGA
};

/* AD9516 output indices for each role (OUT0..OUT9) */
#define AD9516_OUT_DAC_CLK	1
#define AD9516_OUT_DAC_SYSREF	6
#define AD9516_OUT_FPGA_CLK	9
#define AD9516_OUT_FPGA_SYSREF	7

/* KCU116 I2C mux + Si5328 settings */
#if defined(XPAR_AXI_IIC_MAIN_DEVICE_ID)
#define I2C_DEVICE_ID			XPAR_AXI_IIC_MAIN_DEVICE_ID
#elif defined(XPAR_AXI_IIC_0_DEVICE_ID)
#define I2C_DEVICE_ID			XPAR_AXI_IIC_0_DEVICE_ID
#else
#error "I2C device ID not defined in xparameters.h"
#endif

#define I2C_MUX_ADDRESS			0x74
#define I2C_MUX_SI5328_CHANNEL		0x10
#define SI5328_I2C_ADDRESS		0x69
#define SI5328_CLKIN_FREQ_HZ		114285000U
#define SI5328_CLKOUT_FREQ_HZ		245760000U
#define SI5328_LOCK_TIMEOUT_MS		30000U
#endif
