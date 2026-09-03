# make file for building the bare metal side of microblaze, adapted from the 2021.2 version present on the analog devices github. 
# the names of the files are sufficient hints on the functions that they perform. More details for this can be found in the codes for this. 
# fmcdac is the main file used to set up the baremetal os libraries. 

SRCS += $(PROJECT)/src/app/fmcdac.c \
        $(PROJECT)/src/app/awg_sched.c \
        $(PROJECT)/src/app/awg_event_validate.c \
        $(PROJECT)/src/app/awg_c1_validate.c \
        $(PROJECT)/src/app/awg_stream_ring.c \
        $(PROJECT)/src/app/awg_stream_proto.c \
        $(DRIVERS)/frequency/ad9516/ad9516.c \
        $(DRIVERS)/si5328/si5328drv.c \
        $(DRIVERS)/axi_core/axi_dac_core/axi_dac_core.c \
        $(DRIVERS)/axi_core/axi_dmac/axi_dmac.c \
        $(DRIVERS)/axi_core/clk_axi_clkgen/clk_axi_clkgen.c \
        $(DRIVERS)/axi_core/jesd204/axi_adxcvr.c \
        $(DRIVERS)/axi_core/jesd204/axi_jesd204_tx.c \
        $(DRIVERS)/axi_core/jesd204/xilinx_transceiver.c \
        $(DRIVERS)/dac/ad9144/ad9144.c \
        $(DRIVERS)/api/no_os_spi.c \
        $(DRIVERS)/api/no_os_gpio.c \
        $(NO-OS)/util/no_os_util.c \
        $(NO-OS)/jesd204/jesd204-core.c \
        $(NO-OS)/jesd204/jesd204-fsm.c

ifeq ($(FMCDAC_AWG_SCHED_DMA_REFILL),1)
SRCS += $(PROJECT)/src/app/awg_sched_dma.c \
        $(DRIVERS)/api/no_os_irq.c \
        $(PLATFORM_DRIVERS)/xilinx_irq.c
endif

ifeq ($(FMCDAC_AWG_SCHED_ETH),1)
SRCS += $(PROJECT)/src/app/awg_phase_f.c \
        $(PROJECT)/src/app/awg_eth_mac.c \
        $(PROJECT)/src/app/awg_eth_rx.c \
        $(PROJECT)/src/app/awg_eth_tx.c \
        $(PROJECT)/src/app/awg_net.c
endif

SRCS += $(PLATFORM_DRIVERS)/xilinx_axi_io.c \
        $(PLATFORM_DRIVERS)/xilinx_spi.c \
        $(PLATFORM_DRIVERS)/xilinx_gpio.c \
        $(PLATFORM_DRIVERS)/xilinx_delay.c

INCS += $(PROJECT)/src/app/app_config.h \
        $(PROJECT)/src/app/awg_sched.h \
        $(PROJECT)/src/app/awg_sched_regs.h \
        $(PROJECT)/src/app/awg_event.h \
        $(PROJECT)/src/app/awg_event_validate.h \
        $(PROJECT)/src/app/awg_c1_validate.h \
        $(PROJECT)/src/app/awg_stream_ring.h \
        $(PROJECT)/src/app/awg_stream_proto.h \
        $(PROJECT)/src/app/parameters.h \
        $(DRIVERS)/axi_core/axi_dac_core/axi_dac_core.h \
        $(DRIVERS)/axi_core/axi_dmac/axi_dmac.h \
        $(DRIVERS)/axi_core/clk_axi_clkgen/clk_axi_clkgen.h \
        $(DRIVERS)/axi_core/jesd204/axi_adxcvr.h \
        $(DRIVERS)/axi_core/jesd204/axi_jesd204_tx.h \
        $(DRIVERS)/axi_core/jesd204/xilinx_transceiver.h \
        $(DRIVERS)/io-expander/demux_spi.h \
        $(DRIVERS)/frequency/ad9516/ad9516.h \
        $(DRIVERS)/frequency/ad9516/ad9516_cfg.h \
        $(DRIVERS)/si5328/si5328drv.h \
        $(DRIVERS)/dac/ad9144/ad9144.h

ifeq ($(FMCDAC_AWG_SCHED_DMA_REFILL),1)
INCS += $(PROJECT)/src/app/awg_sched_dma.h \
        $(INCLUDE)/no_os_irq.h \
        $(PLATFORM_DRIVERS)/xilinx_irq.h
endif

ifeq ($(FMCDAC_AWG_SCHED_ETH),1)
INCS += $(PROJECT)/src/app/awg_phase_f.h \
        $(PROJECT)/src/app/awg_eth_mac.h \
        $(PROJECT)/src/app/awg_eth_rx.h \
        $(PROJECT)/src/app/awg_eth_tx.h \
        $(PROJECT)/src/app/awg_net.h
endif

INCS += $(PLATFORM_DRIVERS)/$(PLATFORM)_spi.h \
        $(PLATFORM_DRIVERS)/$(PLATFORM)_gpio.h

INCS += $(INCLUDE)/no_os_axi_io.h \
        $(INCLUDE)/no_os_spi.h \
        $(INCLUDE)/no_os_gpio.h \
        $(INCLUDE)/no_os_error.h \
        $(INCLUDE)/no_os_delay.h \
        $(INCLUDE)/no_os_util.h \
        $(INCLUDE)/no_os_print_log.h \
        $(INCLUDE)/jesd204.h \
        $(NO-OS)/jesd204/jesd204-priv.h \
        $(INCLUDE)/no_os_alloc.h
