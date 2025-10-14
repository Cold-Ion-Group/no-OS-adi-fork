#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include <xil_printf.h>
#include <xil_cache.h>
#include "app_config.h"
#include "parameters.h"
#include "axi_adxcvr.h"
#include "no_os_spi.h"
#include "no_os_gpio.h"
#include "xilinx_spi.h"
#include "xilinx_gpio.h"
#include "no_os_delay.h"
#include "no_os_error.h"
#include "ad9144.h"
#include "ad9516.h"
#include "axi_dac_core.h"
#include "axi_dmac.h"
#include "axi_jesd204_tx.h"

// no IIO support included for now

#ifdef JESD_FSM_ON
#include "no_os_print_log.h"
#include "jesd204.h"
#endif
struct fmcdac_dev {
	struct ad9144_dev *ad9144_device;
    struct ad9516_dev *ad9516_device; // Fixed name to match usage

	struct no_os_gpio_desc *gpio_clkd_sync;
	struct no_os_gpio_desc *gpio_dac_reset;
	struct no_os_gpio_desc *gpio_dac_txen;

	struct ad9516_lvpecl_channel_spec ad9516_channels[4]; // Added missing array

	struct adxcvr *ad9144_xcvr;
	struct adxcvr *ad9156_adxcvr; 
	

	struct axi_jesd204_tx *ad9144_jesd;
	struct axi_dac	*ad9144_core;
	struct axi_dac_channel	ad9144_channels[4];  // TODO: modified the number of channels

	struct axi_dmac *ad9144_dmac;
	
} fmcdac;

#ifdef JESD_FSM_ON
struct link_init_param {
	uint32_t	link_id;
	uint32_t	device_id;
	uint32_t	octets_per_frame;
	uint32_t	frames_per_multiframe;
	uint32_t	samples_per_converter_per_frame;
	uint32_t	high_density;
	uint8_t		scrambling;
	uint32_t	converter_resolution;
	uint32_t	bits_per_sample;l
	uint32_t	converters_per_device;
	uint32_t	control_bits_per_sample;
	uint32_t	lanes_per_device;
	uint32_t	subclass;
	uint32_t	version;
	uint8_t		logical_lane_mapping[8];
	/* JTX */
	uint8_t		link_converter_select[16];
	/* JRX */
	uint32_t	tpl_phase_adjust;
};
#endif

struct fmcdac_init_param {

	struct ad9144_init_param ad9144_param;
	struct ad9516_init_param ad9516_param;

	struct adxcvr_init ad9144_xcvr_param;
	struct adxcvr_init ad9516_xcvr_param;

	struct jesd204_tx_init ad9144_jesd_param;
	struct axi_dac_init ad9144_core_param;

	struct axi_dmac_init ad9144_dmac_param;

#ifdef JESD_FSM_ON
	struct link_init_param	jrx_link_tx;
#endif
} fmcdac_init;

static int fmcdac_gpio_init(struct fmcdac_dev *dev)
{
	int status;

	/* Initialize GPIO structures */
	struct no_os_gpio_init_param gpio_clkd_sync_param = {
		.number = GPIO_CLKD_SYNC,
		.platform_ops = &xil_gpio_ops
	};
	struct no_os_gpio_init_param gpio_dac_reset_param = {
		.number = GPIO_DAC_RESET,
		.platform_ops = &xil_gpio_ops
	};
	struct no_os_gpio_init_param gpio_dac_txen_param = {
		.number = GPIO_DAC_TXEN,
		.platform_ops = &xil_gpio_ops
	};

// removed the altera dependencies as that is nto required here
// Microblaze is the only Xilinx processor being used here. 
	struct xil_gpio_init_param xil_gpio_param = {
		.type = GPIO_PL,
		.device_id = GPIO_DEVICE_ID
	};
	gpio_clkd_sync_param.extra = &xil_gpio_param;
	gpio_dac_reset_param.extra = &xil_gpio_param;
	gpio_dac_txen_param.extra = &xil_gpio_param;

	/* set GPIOs */
	status = no_os_gpio_get(&dev->gpio_clkd_sync, &gpio_clkd_sync_param);
	if (status < 0)
		return status;

	status = no_os_gpio_get(&dev->gpio_dac_reset, &gpio_dac_reset_param);
	if (status < 0)
		return status;

	status = no_os_gpio_get(&dev->gpio_dac_txen, &gpio_dac_txen_param);
	if (status < 0)
		return status;


	status = no_os_gpio_direction_output(dev->gpio_clkd_sync, NO_OS_GPIO_LOW);
	if (status < 0)
		return status;

	status = no_os_gpio_direction_output(dev->gpio_dac_reset, NO_OS_GPIO_LOW);
	if (status < 0)
		return status;

	status = no_os_gpio_direction_output(dev->gpio_dac_txen, NO_OS_GPIO_LOW);
	if (status < 0)
		return status;


	no_os_mdelay(5);

	status = no_os_gpio_set_value(dev->gpio_clkd_sync, NO_OS_GPIO_HIGH);
	if (status < 0)
		return status;

	status = no_os_gpio_set_value(dev->gpio_dac_reset, NO_OS_GPIO_HIGH);
	if (status < 0)
		return status;

	status = no_os_gpio_set_value(dev->gpio_dac_txen, NO_OS_GPIO_HIGH);
	if (status < 0)
		return status;

	return 0;
}

static int fmcdac_spi_init(struct fmcdac_init_param *dev_init)
{     // spc matches the 20 mhz, which is present in the data sheet  
// max value is 25 Mhz  
	/* Initialize SPI structures */
	
	struct no_os_spi_init_param ad9516_spi_param = {
		.device_id = SPI_DEVICE_ID,
		.max_speed_hz = 2000000u, // this is the communication speed for the spi 
		.chip_select = 4, // value takem from the evaluation board schematic
	.mode = NO_OS_SPI_MODE_0,
	.platform_ops = &xil_spi_ops
	};

	struct no_os_spi_init_param ad9144_spi_param = {
		.device_id = SPI_DEVICE_ID,
		.max_speed_hz = 2000000u,
		.chip_select = 1,
		.mode = NO_OS_SPI_MODE_0,
		.platform_ops = &xil_spi_ops
	};


	static struct xil_spi_init_param xil_spi_param = {
		.type = SPI_PL,
	};
	ad9516_spi_param.platform_ops = &xil_spi_ops;
	ad9516_spi_param.extra = &xil_spi_param;

	ad9144_spi_param.platform_ops = &xil_spi_ops;
	ad9144_spi_param.extra = &xil_spi_param;

    dev_init->ad9516_param.spi_init = ad9516_spi_param;
	dev_init->ad9144_param.spi_init = ad9144_spi_param;


	return 0;
}



static int fmcdac_clk_init(struct fmcdac_dev *dev,
			    struct fmcdac_init_param *dev_init)
{
	static struct ad9516_platform_data ad9516_pdata;
	int ret;

	// clock distribution device (AD9516) configuration 
	ad9516_pdata.num_channels = 4; // 4 channels as 4 clock lines are being used to run the ad9144 evaluation board
	ad9516_pdata.channels = &dev->ad9516_channels[0]; // channel initialisation may not be of the correct data type
	dev_init->ad9516_param.ad9516_st.pdata = &ad9516_pdata;
	dev_init->ad9516_param.ad9516_type = AD9516_1; // look into  the board and then change this , according to the specs sheet its ad9516-1
	dev_init->ad9516_param.ad9516_st.lvpecl_channels = &dev->ad9516_channels[0];
		// VCXO 125MHz
	  	ad9516_pdata.ref_1_freq = 30720000; // this value may not affect the evaluation board as there is no connection present int he schematic of the evaluation board 
		ad9516_pdata.ref_2_freq = 0;
		ad9516_pdata.diff_ref_en = 0;
		ad9516_pdata.ref_1_power_on = 1;
		ad9516_pdata.ref_2_power_on = 0;
		ad9516_pdata.ref_sel_pin_en = 0;
		ad9516_pdata.ref_sel_pin = 1;
		ad9516_pdata.ref_2_en = 0;
		ad9516_pdata.ext_clk_freq = 0;
		ad9516_pdata.int_vco_freq = 1250000000;
		ad9516_pdata.vco_clk_sel = 1;
		ad9516_pdata.power_down_vco_clk = 0;
		strncpy((char*)ad9516_pdata.name, "ad9516_lpc", sizeof(ad9516_pdata.name) - 1);
		ad9516_pdata.name[sizeof(ad9516_pdata.name) - 1] = '\0';
		
	dev->ad9516_channels[DAC_DEVICE_CLK].channel_num = 0;
	dev->ad9516_channels[DAC_DEVICE_CLK].out_invert_en = 0;
    dev->ad9516_channels[DAC_DEVICE_CLK].out_diff_voltage= LVPECL_780mV;


	dev->ad9516_channels[DAC_DEVICE_SYSREF].channel_num = 1;
	dev->ad9516_channels[DAC_DEVICE_SYSREF].out_invert_en = 0;
    dev->ad9516_channels[DAC_DEVICE_SYSREF].out_diff_voltage= LVPECL_780mV;

	dev->ad9516_channels[DAC_FPGA_CLK].channel_num = 2;
	dev->ad9516_channels[DAC_FPGA_CLK].out_invert_en = 0;
	dev->ad9516_channels[DAC_FPGA_CLK].out_diff_voltage= LVPECL_780mV;

	dev->ad9516_channels[DAC_FPGA_SYSREF].channel_num = 3;
	dev->ad9516_channels[DAC_FPGA_SYSREF].out_invert_en = 0;
	dev->ad9516_channels[DAC_FPGA_SYSREF].out_diff_voltage= LVPECL_780mV;

	ret = ad9516_setup(&dev->ad9516_device, dev_init->ad9516_param);
	
	if (ret < 0) {
		printf("\nClock init failed");
		return ret;
	}


	return ret;
}
// TODO: modified the jesd204 platform to include only ad9144
static int fmcdac_jesd_init(struct fmcdac_init_param *dev_init)
{
	dev_init->ad9144_xcvr_param = (struct adxcvr_init) {
		.name = "ad9144_xcvr",
		.base = TX_XCVR_BASEADDR,
		.sys_clk_sel = ADXCVR_SYS_CLK_QPLL0,
		.out_clk_sel = ADXCVR_REFCLK_DIV2,
		.lpm_enable = 1,
		.ref_rate_khz = 500000,
		.lane_rate_khz = 10000000,
	};
	/* JESD initialization */
	dev_init->ad9144_jesd_param = (struct jesd204_tx_init) {
		.name = "ad9144_jesd",
		.base = TX_JESD_BASEADDR,
		.octets_per_frame = 1,
		.frames_per_multiframe = 32,
		.converters_per_device = 2,
		.converter_resolution = 14,
		.bits_per_sample = 16,
		.high_density = false,
		.control_bits_per_sample = 0,
		.subclass = 1,
		.device_clk_khz = 10000000/40,
		.lane_clk_khz = 10000000
	};
#ifdef JESD_FSM_ON
	struct link_init_param jrx_link_tx = {
		.link_id = 1,
		.device_id = 0,
		.octets_per_frame = 1,
		.frames_per_multiframe = 32,
		.samples_per_converter_per_frame = 1,
		.scrambling = 0,
		.high_density = 0,
		.converter_resolution = 14,
		.bits_per_sample = 16,
		.converters_per_device = 2,
		.control_bits_per_sample = 2,
		.lanes_per_device = 4,
		.subclass = 1,
		.version = 2,
	};
	fmcdac_init.jrx_link_tx = jrx_link_tx;
#endif

	return 0;
}

static int fmcdac_trasnceiver_setup(struct fmcdac_dev *dev,
				     struct fmcdac_init_param *dev_init)
{
	int status;

#ifdef JESD_FSM_ON
	status = axi_jesd204_tx_init_jesd_fsm(&dev->ad9144_jesd,
					      &dev_init->ad9144_jesd_param);
	if (status) {
		printf("error: %s: axi_jesd204_tx_init_jesd_fsm() failed\n",
		       dev_init->ad9144_jesd_param.name);
		return status;
	}
#else
	status = axi_jesd204_tx_init(&dev->ad9144_jesd, &dev_init->ad9144_jesd_param);
	if (status != 0) {
		printf("error: %s: axi_jesd204_tx_init() failed\n",
		       dev_init->ad9144_jesd_param.name);
		return status;
	}

	status = axi_jesd204_tx_lane_clk_enable(dev->ad9144_jesd);
	if (status != 0) {
		printf("error: %s: axi_jesd204_tx_lane_clk_enable() failed\n",
		       dev->ad9144_jesd->name);
		return status;
	}
#endif

	status = adxcvr_init(&dev->ad9144_xcvr, &dev_init->ad9144_xcvr_param);
	if (status != 0) {
		printf("error: %s: adxcvr_init() failed\n", dev_init->ad9144_xcvr_param.name);
		return status;
	}
#ifndef ALTERA_PLATFORM
	status = adxcvr_clk_enable(dev->ad9144_xcvr);
	if (status != 0) {
		printf("error: %s: adxcvr_clk_enable() failed\n", dev->ad9144_xcvr->name);
		return status;
	}
#endif
	return status;
}


static int fmcdac_test(struct fmcdac_dev *dev,
			struct fmcdac_init_param *dev_init)
{
	int status;


	status = axi_jesd204_tx_status_read(dev->ad9144_jesd);
	if (status != 0) {
		printf("axi_jesd204_tx_status_read() error: %d\n", status);
	}

	status = ad9144_status(dev->ad9144_device);
	if (status == -1)
		return status;

	/* transport path testing, enabling all 4 channels */
	dev->ad9144_channels[0].sel = AXI_DAC_DATA_SEL_SED;
	dev->ad9144_channels[1].sel = AXI_DAC_DATA_SEL_SED;
	dev->ad9144_channels[2].sel = AXI_DAC_DATA_SEL_SED;
	dev->ad9144_channels[3].sel = AXI_DAC_DATA_SEL_SED;
	axi_dac_data_setup(dev->ad9144_core);
	ad9144_short_pattern_test(dev->ad9144_device, &dev_init->ad9144_param);

	// PN7 data path test

	dev->ad9144_channels[0].sel = AXI_DAC_DATA_SEL_PN23;
	dev->ad9144_channels[1].sel = AXI_DAC_DATA_SEL_PN23;
	dev->ad9144_channels[2].sel = AXI_DAC_DATA_SEL_PN23;
	dev->ad9144_channels[3].sel = AXI_DAC_DATA_SEL_PN23;
	axi_dac_data_setup(dev->ad9144_core);
	dev_init->ad9144_param.prbs_type = AD9144_PRBS7;
	ad9144_datapath_prbs_test(dev->ad9144_device, &dev_init->ad9144_param);

	// PN15 data path test
//TODO: set it for 4 channels 
	dev->ad9144_channels[0].sel = AXI_DAC_DATA_SEL_PN31;
	dev->ad9144_channels[1].sel = AXI_DAC_DATA_SEL_PN31;
	dev->ad9144_channels[2].sel = AXI_DAC_DATA_SEL_PN31;
	dev->ad9144_channels[3].sel = AXI_DAC_DATA_SEL_PN31;

	axi_dac_data_setup(dev->ad9144_core);
	dev_init->ad9144_param.prbs_type = AD9144_PRBS15;
	ad9144_datapath_prbs_test(dev->ad9144_device, &dev_init->ad9144_param);


return 0;
}

static int fmcdac_dac_init(struct fmcdac_dev *dev,
			    struct fmcdac_init_param *dev_init)
{
	/* DAC (AD9144) channels configuration */
	dev->ad9144_channels[0].pat_data = 0xb1b0a1a0;
	dev->ad9144_channels[1].pat_data = 0xd1d0c1c0;
	dev->ad9144_channels[2].pat_data = 0xb1b0a1a0;
	dev->ad9144_channels[3].pat_data = 0xd1d0c1c0;


  	dev->ad9144_channels[0].sel = AXI_DAC_DATA_SEL_DDS;
	dev->ad9144_channels[1].sel = AXI_DAC_DATA_SEL_DDS;
	dev->ad9144_channels[2].sel = AXI_DAC_DATA_SEL_DDS;
	dev->ad9144_channels[3].sel = AXI_DAC_DATA_SEL_DDS;


	/* DAC Core */
	dev_init->ad9144_core_param = (struct axi_dac_init) {
		.name = "ad9144_dac",
		.base =	TX_CORE_BASEADDR,
		.num_channels = 4,
		.channels = &dev->ad9144_channels[0]
	};

	for(uint32_t n=0;
	    n < NO_OS_ARRAY_SIZE(dev_init->ad9144_param.lane_mux);
	    n++)
		dev_init->ad9144_param.lane_mux[n] = n;

	dev_init->ad9144_param.stpl_samples[0][0] =
		(dev->ad9144_channels[0].pat_data >> 0)  & 0xffff;
	dev_init->ad9144_param.stpl_samples[0][1] =
		(dev->ad9144_channels[0].pat_data >> 16) & 0xffff;
	dev_init->ad9144_param.stpl_samples[0][2] =
		(dev->ad9144_channels[0].pat_data >> 0)  & 0xffff;
	dev_init->ad9144_param.stpl_samples[0][3] =
		(dev->ad9144_channels[0].pat_data >> 16) & 0xffff;
	dev_init->ad9144_param.stpl_samples[1][0] =
		(dev->ad9144_channels[1].pat_data >> 0)  & 0xffff;
	dev_init->ad9144_param.stpl_samples[1][1] =
		(dev->ad9144_channels[1].pat_data >> 16) & 0xffff;
	dev_init->ad9144_param.stpl_samples[1][2] =
		(dev->ad9144_channels[1].pat_data >> 0)  & 0xffff;
	dev_init->ad9144_param.stpl_samples[1][3] =
		(dev->ad9144_channels[1].pat_data >> 16) & 0xffff;
		dev_init->ad9144_param.stpl_samples[2][0] =
		(dev->ad9144_channels[2].pat_data >> 0)  & 0xffff;
	dev_init->ad9144_param.stpl_samples[2][1] =
		(dev->ad9144_channels[2].pat_data >> 16) & 0xffff;
	dev_init->ad9144_param.stpl_samples[2][2] =
		(dev->ad9144_channels[2].pat_data >> 0)  & 0xffff;
	dev_init->ad9144_param.stpl_samples[2][3] =
		(dev->ad9144_channels[2].pat_data >> 16) & 0xffff;
		dev_init->ad9144_param.stpl_samples[3][0] =
		(dev->ad9144_channels[3].pat_data >> 0)  & 0xffff;
	dev_init->ad9144_param.stpl_samples[3][1] =
		(dev->ad9144_channels[3].pat_data >> 16) & 0xffff;
	dev_init->ad9144_param.stpl_samples[3][2] =
		(dev->ad9144_channels[3].pat_data >> 0)  & 0xffff;
	dev_init->ad9144_param.stpl_samples[3][3] =
		(dev->ad9144_channels[3].pat_data >> 16) & 0xffff;		
	dev_init->ad9144_jesd_param.device_clk_khz =
		dev_init->ad9144_xcvr_param.lane_rate_khz / 40;
	dev_init->ad9144_jesd_param.lane_clk_khz =
		dev_init->ad9144_xcvr_param.lane_rate_khz ;

	return 0;
}


static void fmcdac_remove(struct fmcdac_dev *dev)
{
	/* Memory deallocation for devices and spi */
	ad9144_remove(dev->ad9144_device);
	ad9516_remove(dev->ad9516_device);

	/* Memory deallocation for PHY and LINK layers */
	adxcvr_remove(dev->ad9144_xcvr);
	axi_jesd204_tx_remove(dev->ad9144_jesd);

	/* Memory deallocation for gpios */
	// additional code for gpio signals needs to be added here
	no_os_gpio_remove(dev->gpio_clkd_sync);
	no_os_gpio_remove(dev->gpio_dac_reset);
	no_os_gpio_remove(dev->gpio_dac_txen);
	// fifo??????? l
}

int fmcdac_reconfig(struct ad9144_init_param *p_ad9144_param,
		     struct adxcvr_init *ad9144_xcvr_param,
		     struct ad9516_platform_data *p_ad9516_param)
{

	uint8_t mode = 0;

	printf ("Available sampling rates:\n");
	printf ("\t1 - DAC 1000 MSPS\n");
	printf ("\t2 - DAC 1000 MSPS\n");
	printf ("\t3 - DAC  500 MSPS\n");
	printf ("\t4 - DAC  600 MSPS\n");
	printf ("\t5 - DAC 2000 MSPS (2x interpolation)\n");
	printf ("choose an option [default 1]:\n");

	mode = getc(stdin);
    // TODO: look into the modification for how the clock is configured and for replacing ad9523
	switch (mode) {
	case '5':
		printf("5 - DAC 2000 MSPS (2x interpolation)\n");
		/* REF clock = 100 MHz */
		//p_ad9516_param->channels[DAC_DEVICE_CLK].channel_divider = 10;
		p_ad9144_param->pll_ref_frequency_khz = 100000;

		/* DAC at 2 GHz using the internal PLL and 2 times interpolation */
		p_ad9144_param->interpolation = 2;
		p_ad9144_param->pll_enable = 1;
		p_ad9144_param->pll_dac_frequency_khz = 2000000;
		/* Set SYSCLK_SEL to QPLL */
		//ad9680_xcvr_param->sys_clk_sel = ADXCVR_SYS_CLK_QPLL0;
		break;
	case '4':
		printf ("DAC  600 MSPS\n");

		p_ad9144_param->lane_rate_kbps = 6000000;
		ad9144_xcvr_param->lane_rate_khz = 6000000;
#ifndef ALTERA_PLATFORM
		ad9144_xcvr_param->ref_rate_khz = 300000;
#else
		ad9144_xcvr_param->parent_rate_khz = 300000;
#endif

#ifndef ALTERA_PLATFORM
		//ad9680_xcvr_param->ref_rate_khz = 300000;
#else
		//ad9680_xcvr_param->parent_rate_khz = 300000;
#endif
#ifndef ALTERA_PLATFORM
		ad9144_xcvr_param->lpm_enable = 0;
		ad9144_xcvr_param->sys_clk_sel = ADXCVR_SYS_CLK_CPLL;
		ad9144_xcvr_param->out_clk_sel = ADXCVR_REFCLK_DIV2;

		//ad9680_xcvr_param->lpm_enable = 1;
		//ad9680_xcvr_param->sys_clk_sel = ADXCVR_SYS_CLK_CPLL;
		//ad9680_xcvr_param->out_clk_sel = ADXCVR_REFCLK_DIV2;
#endif
		break;
	case '3':
		printf ("3 - DAC  500 MSPS\n");

		p_ad9144_param->lane_rate_kbps = 5000000;
		ad9144_xcvr_param->lane_rate_khz = 5000000;
#ifndef ALTERA_PLATFORM
		ad9144_xcvr_param->ref_rate_khz = 250000;
#else
		ad9144_xcvr_param->parent_rate_khz = 250000;
#endif

#ifndef ALTERA_PLATFORM
#else
#endif
#ifndef ALTERA_PLATFORM
		ad9144_xcvr_param->sys_clk_sel = ADXCVR_SYS_CLK_CPLL;
		ad9144_xcvr_param->lpm_enable = 1;
		ad9144_xcvr_param->out_clk_sel = ADXCVR_REFCLK_DIV2;


#endif
		break;
	case '2':
		printf ("2 - DAC 1000 MSPS\n");

		p_ad9144_param->lane_rate_kbps = 10000000;
		ad9144_xcvr_param->lane_rate_khz = 10000000;
		ad9144_xcvr_param->ref_rate_khz = 500000;

		break;
	default:
		printf ("1 - DAC 1000 MSPS\n");
		ad9144_xcvr_param->ref_rate_khz = 500000;
		break;
	}

	return(0);
}

static int fmcdac_setup(struct fmcdac_dev *dev,
			 struct fmcdac_init_param *dev_init)
{
	int status;

	status = fmcdac_gpio_init(dev);
	if (status < 0)
		return status;

	status = fmcdac_spi_init(dev_init);
	if (status < 0)
		return status;

	status = fmcdac_clk_init(dev, dev_init);
	if (status < 0)
		return status;

	status = fmcdac_jesd_init(dev_init);
	if (status < 0)
		return status;

	dev_init->ad9144_param.lane_rate_kbps = 10000000;
	dev_init->ad9144_param.spi3wire = 1;
#ifdef JESD_FSM_ON
	dev_init->ad9144_param.num_converters =
		fmcdac_init.jtx_link_rx.converters_per_device;
	dev_init->ad9144_param.num_lanes = fmcdac_init.jtx_link_rx.lanes_per_device;
#endif
	dev_init->ad9144_param.interpolation = 1;
	dev_init->ad9144_param.fcenter_shift = 0;
	dev_init->ad9144_param.pll_enable = 0;
	dev_init->ad9144_param.jesd204_subclass = 1;
	dev_init->ad9144_param.jesd204_scrambling = 1;
	dev_init->ad9144_param.jesd204_mode = 4;


	/* change the default JESD configurations, if required */
	fmcdac_reconfig(&dev_init->ad9144_param,
			 &dev_init->ad9144_xcvr_param,
			 &dev_init->ad9516_param.ad9516_st.pdata);

	status = fmcdac_dac_init(&fmcdac, &fmcdac_init);
	if (status < 0)
		return status;

	/* Reconfigure the default JESD configurations */

	/* setup clocks */
	status = ad9516_setup(&dev->ad9516_device, dev_init->ad9516_param);
	if (status != 0) {
		printf("error: ad9516_setup() failed\n");
		return status;
	}
	// Recommended DAC JESD204 link startup sequence
	//   1. FPGA JESD204 Link Layer
	//   2. FPGA JESD204 PHY Layer
	//   3. DAC
	//




	status = fmcdac_trasnceiver_setup(&fmcdac, &fmcdac_init);
	if (status != 0)
		return status;

#ifdef JESD_FSM_ON
	status = ad9144_setup_jesd_fsm(&dev->ad9144_device, &dev_init->ad9144_param);
	if (status) {
		printf("error: ad9144_setup_jesd_fsm() failed\n");
		return status;
	}
#else
	status = ad9144_setup_legacy(&dev->ad9144_device, &dev_init->ad9144_param);
	if (status != 0) {
		printf("error: ad9144_setup_legacy() failed\n");
		return status;
	}
#endif

#ifdef JESD_FSM_ON
	dev->ad9144_device->link_config.is_transmit = true;
	dev->ad9144_device->link_config.link_id = fmcdac_init.jtx_link_rx.link_id;
	dev->ad9144_device->link_config.bank_id = 0;
	dev->ad9144_device->link_config.device_id = fmcdac_init.jtx_link_rx.device_id;
	dev->ad9144_device->link_config.octets_per_frame =
		fmcdac_init.jtx_link_rx.octets_per_frame;
	dev->ad9144_device->link_config.frames_per_multiframe =
		fmcdac_init.jtx_link_rx.frames_per_multiframe;
	dev->ad9144_device->link_config.samples_per_conv_frame =
		fmcdac_init.jtx_link_rx.samples_per_converter_per_frame;
	dev->ad9144_device->link_config.high_density =
		fmcdac_init.jtx_link_rx.high_density;
	dev->ad9144_device->link_config.scrambling =
		fmcdac_init.jtx_link_rx.scrambling;
	dev->ad9144_device->link_config.converter_resolution =
		fmcdac_init.jtx_link_rx.converter_resolution;
	dev->ad9144_device->link_config.num_converters =
		fmcdac_init.jtx_link_rx.converters_per_device;
	dev->ad9144_device->link_config.bits_per_sample =
		fmcdac_init.jtx_link_rx.bits_per_sample;
	dev->ad9144_device->link_config.ctrl_bits_per_sample =
		fmcdac_init.jtx_link_rx.control_bits_per_sample;
	dev->ad9144_device->link_config.num_lanes =
		fmcdac_init.jtx_link_rx.lanes_per_device;
	dev->ad9144_device->link_config.subclass = fmcdac_init.jtx_link_rx.subclass;
	dev->ad9144_device->link_config.jesd_version = fmcdac_init.jtx_link_rx.version;

	dev->ad9144_device->link_config.sysref.capture_falling_edge = 1;
	dev->ad9144_device->link_config.sysref.mode = JESD204_SYSREF_ONESHOT;

	dev->ad9144_device->link_config.lane_ids = calloc(
				dev->ad9144_device->link_config.num_lanes, sizeof(uint8_t));
	if (!dev->ad9144_device->link_config.lane_ids)
		return -ENOMEM;

	for (int lane = 0; lane < dev->ad9144_device->link_config.num_lanes; lane++)
		dev->ad9144_device->link_config.lane_ids[lane] = lane;
#endif

	status = axi_dac_init(&dev->ad9144_core, &dev_init->ad9144_core_param);
	if (status != 0) {
		printf("axi_dac_init() error: %s\n", dev_init->ad9144_core_param.name);
		return status;
	}

	return status;
}

int main(void)
{
	//unsigned int *data = (unsigned int *)ADC_DDR_BASEADDR;
	int status;

	status = fmcdac_setup(&fmcdac, &fmcdac_init);
	if (status < 0)
		return status;

#ifdef JESD_FSM_ON
	pr_info("Using JESD FSM.\n");


	struct jesd204_topology *topology_tx;
	
	struct jesd204_topology_dev devs_tx[] = {
		{
			.jdev = fmcdac.ad9144_jesd->jdev,
			.link_ids = {1},
			.links_number = 1,
		},
		{
			.jdev = fmcdac.ad9144_device->jdev,
			.link_ids = {1},
			.links_number = 1,
			.is_top_device = true,
		},
	};

	jesd204_topology_init(&topology, devs,
			      sizeof(devs) / sizeof(*devs));

	jesd204_topology_init(&topology_tx, devs_tx,
			      sizeof(devs_tx) / sizeof(*devs_tx));

	jesd204_fsm_start(topology, JESD204_LINKS_ALL);
	jesd204_fsm_start(topology_tx, JESD204_LINKS_ALL);
#endif

	fmcdac_test(&fmcdac, &fmcdac_init);



	/* DAC DMA Example */
#ifdef DAC_DMA_EXAMPLE
	extern const uint32_t sine_lut_iq[1024];
	fmcdac_init.ad9144_dmac_param = (struct axi_dmac_init) {
		.name = "tx_dmac",
		.base = TX_DMA_BASEADDR,
		.irq_option = IRQ_DISABLED
	};
	fmcdac.ad9144_channels[0].sel = AXI_DAC_DATA_SEL_DMA;
	fmcdac.ad9144_channels[1].sel = AXI_DAC_DATA_SEL_DMA;
	fmcdac.ad9144_channels[2].sel = AXI_DAC_DATA_SEL_DMA;
	fmcdac.ad9144_channels[3].sel = AXI_DAC_DATA_SEL_DMA;

#ifdef USE_NCO
	status = ad9144_set_nco(fmcdac.ad9144_device,62500,1);
	if (status)
		return status;
#endif
	status = axi_dmac_init(&fmcdac.ad9144_dmac, &fmcdac_init.ad9144_dmac_param);
	if (status)
		return status;
	axi_dac_data_setup(fmcdac.ad9144_core);
	axi_dac_load_custom_data(fmcdac.ad9144_core, sine_lut_iq,
				 NO_OS_ARRAY_SIZE(sine_lut_iq), DAC_DDR_BASEADDR);
#ifndef ALTERA_PLATFORM
	Xil_DCacheFlush();
#endif
	struct axi_dma_transfer transfer_tx = {
		// Number of bytes to write/read
		.size = NO_OS_ARRAY_SIZE(sine_lut_iq) * sizeof(uint32_t),
		// Transfer done flag
		.transfer_done = 0,
		// Signal transfer mode
		.cyclic = NO,
		// Address of data source
		.src_addr = (uintptr_t)DAC_DDR_BASEADDR,
		// Address of data destination
		.dest_addr = 0
	};
	status = axi_dmac_transfer_start(fmcdac.ad9144_dmac, &transfer_tx);
	if (status)
		return status;
#ifndef ALTERA_PLATFORM
	Xil_DCacheInvalidateRange((uintptr_t)DAC_DDR_BASEADDR,
				  NO_OS_ARRAY_SIZE(sine_lut_iq) * sizeof(uint32_t));
#endif
#else
	fmcdac.ad9144_channels[0].dds_dual_tone = 0;
	fmcdac.ad9144_channels[0].dds_frequency_0 = 33*1000*1000;
	fmcdac.ad9144_channels[0].dds_phase_0 = 0;
	fmcdac.ad9144_channels[0].dds_scale_0 = 500000;
	fmcdac.ad9144_channels[0].sel = AXI_DAC_DATA_SEL_DDS;
	fmcdac.ad9144_channels[1].dds_dual_tone = 0;
	fmcdac.ad9144_channels[1].dds_frequency_0 = 11*1000*1000;
	fmcdac.ad9144_channels[1].dds_phase_0 = 0;
	fmcdac.ad9144_channels[1].dds_scale_0 = 500000;
	fmcdac.ad9144_channels[1].sel = AXI_DAC_DATA_SEL_DDS;
	fmcdac.ad9144_channels[2].dds_dual_tone = 0;
	fmcdac.ad9144_channels[2].dds_frequency_0 = 33*1000*1000;
	fmcdac.ad9144_channels[2].dds_phase_0 = 0;
	fmcdac.ad9144_channels[2].dds_scale_0 = 500000;
	fmcdac.ad9144_channels[2].sel = AXI_DAC_DATA_SEL_DDS;
	fmcdac.ad9144_channels[3].dds_dual_tone = 0;
	fmcdac.ad9144_channels[3].dds_frequency_0 = 11*1000*1000;
	fmcdac.ad9144_channels[3].dds_phase_0 = 0;
	fmcdac.ad9144_channels[3].dds_scale_0 = 500000;
	fmcdac.ad9144_channels[3].sel = AXI_DAC_DATA_SEL_DDS;
	axi_dac_data_setup(fmcdac.ad9144_core);
#endif

	fmcdac_remove(&fmcdac);

	return 0;
}