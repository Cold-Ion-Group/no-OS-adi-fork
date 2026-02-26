################################################################################
#									       #
#     Shared variables:							       #
#	- PROJECT							       #
#	- DRIVERS							       #
#	- INCLUDE							       #
#	- PLATFORM_DRIVERS						       #
#	- NO-OS								       #
#									       #
################################################################################

# Uncomment to select the profile

# modified it to include only libraries relevant to ad9144, modified by Prerna Baranwal for KCU116 and AD9144  

SRCS += $(PROJECT)/src/app/fmcdac.c		
SRCS +=	$(DRIVERS)/frequency/ad9516/ad9516.c			
SRCS += $(DRIVERS)/axi_core/axi_dac_core/axi_dac_core.c \
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
SRCS +=	$(PLATFORM_DRIVERS)/xilinx_axi_io.c \
	$(PLATFORM_DRIVERS)/xilinx_spi.c \
	$(PLATFORM_DRIVERS)/xilinx_gpio.c \
	$(PLATFORM_DRIVERS)/xilinx_delay.c
ifeq (y,$(strip $(TINYIIOD)))
LIBRARIES += iio
SRC_DIRS += $(NO-OS)/iio/iio_app
SRCS += $(NO-OS)/util/no_os_fifo.c \
	$(NO-OS)/util/no_os_list.c \
	$(DRIVERS)/dac/ad9144/iio_ad9144.c \
	$(DRIVERS)/axi_core/iio_axi_dac/iio_axi_dac.c \
	$(DRIVERS)/api/no_os_irq.c \
	$(DRIVERS)/api/no_os_uart.c \
	$(PLATFORM_DRIVERS)/$(PLATFORM)_uart.c \
	$(NO-OS)/util/no_os_lf256fifo.c \
	$(PLATFORM_DRIVERS)/$(PLATFORM)_irq.c
endif
INCS +=	$(PROJECT)/src/app/app_config.h \
	$(PROJECT)/src/app/parameters.h
ifeq (y,$(strip $(TINYIIOD)))
INCS +=	$(DRIVERS)/dac/ad9144/iio_ad9144.h
endif
INCS += $(DRIVERS)/axi_core/axi_dac_core/axi_dac_core.h \
	$(DRIVERS)/axi_core/axi_dmac/axi_dmac.h \
	$(DRIVERS)/axi_core/clk_axi_clkgen/clk_axi_clkgen.h \
	$(DRIVERS)/axi_core/jesd204/axi_adxcvr.h \
	$(DRIVERS)/axi_core/jesd204/axi_jesd204_tx.h \
	$(DRIVERS)/axi_core/jesd204/xilinx_transceiver.h \
	$(DRIVERS)/io-expander/demux_spi.h \
	$(DRIVERS)/frequency/ad9516/ad9516.h \
	$(DRIVERS)/dac/ad9144/ad9144.h					
INCS +=	$(PLATFORM_DRIVERS)/$(PLATFORM)_spi.h \
	$(PLATFORM_DRIVERS)/$(PLATFORM)_gpio.h
INCS +=	$(INCLUDE)/no_os_axi_io.h \
	$(INCLUDE)/no_os_spi.h \
	$(INCLUDE)/no_os_gpio.h \
	$(INCLUDE)/no_os_error.h \
	$(INCLUDE)/no_os_delay.h \
	$(INCLUDE)/no_os_util.h \
	$(INCLUDE)/no_os_print_log.h \
	$(INCLUDE)/jesd204.h \
	$(NO-OS)/jesd204/jesd204-priv.h
ifeq (y,$(strip $(TINYIIOD)))
INCS += $(INCLUDE)/no_os_fifo.h \
	$(INCLUDE)/no_os_irq.h \
	$(INCLUDE)/no_os_uart.h \
	$(INCLUDE)/no_os_lf256fifo.h \
	$(INCLUDE)/no_os_list.h \
	$(PLATFORM_DRIVERS)/$(PLATFORM)_irq.h \
	$(PLATFORM_DRIVERS)/$(PLATFORM)_uart.h \
	$(DRIVERS)/axi_core/iio_axi_dac/iio_axi_dac.h
endif

#$(DRIVERS)/frequency/ad9516/ad9516.c problematic!!