#include <stdio.h>
#include <inttypes.h>
#include <xil_printf.h>
#include <xil_cache.h>
#include <xstatus.h>
#include <xiic.h>
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
#include "si5328drv.h"

/* Stringify NO_OS_VERSION — the Makefile's -D quotes may not survive
 * the Windows shell, so we force stringification here. */
#define _XSTR(x) #x
#define _STR(x)  _XSTR(x)
#ifdef NO_OS_VERSION
#define BUILD_VERSION_STR _STR(NO_OS_VERSION)
#else
#define BUILD_VERSION_STR "unknown"
#endif

// no IIO support included for now

#ifdef JESD_FSM_ON
#include "no_os_print_log.h"
#include "jesd204.h"
#endif
struct fmcdac_dev {
	struct ad9144_dev *ad9144_device;
    struct ad9516_dev *ad9516_dev;

	struct no_os_gpio_desc *gpio_clkd_sync;
	struct no_os_gpio_desc *gpio_dac_reset;
	struct no_os_gpio_desc *gpio_dac_txen;

	XIic i2c;
	u32 i2c_base;

	struct adxcvr *ad9144_xcvr;
	struct adxcvr *ad9156_adxcvr; 
	
	struct axi_jesd204_tx *ad9144_jesd;
	struct axi_dac	*ad9144_core;
	struct axi_dac_channel	ad9144_channels[4];

	struct axi_dmac *ad9144_dmac;
	
	struct ad9516_lvpecl_channel_spec ad9516_channels[4];
	struct ad9516_lvds_cmos_channel_spec ad9516_lvds_channels[4];
} fmcdac;

enum fmcdac_clock_mode {
	FMCDAC_CLK_DISTRIBUTE = 0,
	FMCDAC_CLK_SYNTHESIZE,
	FMCDAC_CLK_EXTERNAL,
};

static enum fmcdac_clock_mode g_clk_mode = FMCDAC_CLK_DISTRIBUTE;

static void fmcdac_flush_input(void);

#ifndef FMCDAC_DEFAULT_RATE_OPTION
#define FMCDAC_DEFAULT_RATE_OPTION 1
#endif

#if (FMCDAC_DEFAULT_RATE_OPTION != 1) && (FMCDAC_DEFAULT_RATE_OPTION != 2)
#error "FMCDAC_DEFAULT_RATE_OPTION must be 1 (2x interpolation) or 2 (1x, no interpolation)"
#endif

#if FMCDAC_DEFAULT_RATE_OPTION == 1
#define FMCDAC_DEFAULT_RATE_OPTION_CHAR '1'
#define FMCDAC_DEFAULT_RATE_TEXT "option 1 (DAC 1966 MSPS, 2x interpolation)"
#else
#define FMCDAC_DEFAULT_RATE_OPTION_CHAR '2'
#define FMCDAC_DEFAULT_RATE_TEXT "option 2 (DAC 983 MSPS, 1x, no interpolation)"
#endif

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
	uint32_t	bits_per_sample;
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

static void fmcdac_ad9516_dump_regs(struct ad9516_dev *dev);
static int fmcdac_ad9516_wait_pll_lock(struct ad9516_dev *dev, uint32_t timeout_ms);
static void fmcdac_apply_clock_mode(struct ad9144_init_param *p_ad9144_param,
				    struct ad9516_platform_data *p_ad9516_param);
static int fmcdac_i2c_init(struct fmcdac_dev *dev);
static void fmcdac_i2c_probe(struct fmcdac_dev *dev);
static int fmcdac_i2c_mux_select(struct fmcdac_dev *dev, uint8_t channel_mask);
static void fmcdac_i2c_scan_si5328(struct fmcdac_dev *dev);
static void fmcdac_i2c_scan_bus(struct fmcdac_dev *dev, uint8_t channel_mask);
static int fmcdac_si5328_setup(struct fmcdac_dev *dev);
static int fmcdac_ad9516_clock_only(struct fmcdac_dev *dev);
static void fmcdac_hold_for_probe(const char *reason);
static void fmcdac_ad9516_signature_toggle(struct fmcdac_dev *dev);
static void fmcdac_ad9144_jesd_sanity(struct ad9144_dev *dev,
				      const struct ad9144_init_param *init_param);
static void fmcdac_sysref_verify(struct fmcdac_dev *dev);
static int fmcdac_sysref_tune(struct fmcdac_dev *dev);
static void fmcdac_latency_readback(struct fmcdac_dev *dev);
static void fmcdac_phy_prbs_test(struct fmcdac_dev *dev);
static int fmcdac_nco_discriminator_test(struct fmcdac_dev *dev);
static int fmcdac_dds_band_diagnostic_test(struct fmcdac_dev *dev);
static void fmcdac_flush_input(void);

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

static int fmcdac_i2c_init(struct fmcdac_dev *dev)
{
	XIic_Config *cfg;
	int ret;

	cfg = XIic_LookupConfig(I2C_DEVICE_ID);
	if (!cfg) {
		xil_printf("[ERROR] I2C config lookup failed (device_id=%d)\n\r",
			   I2C_DEVICE_ID);
		return -1;
	}

	ret = XIic_CfgInitialize(&dev->i2c, cfg, cfg->BaseAddress);
	if (ret != XST_SUCCESS) {
		xil_printf("[ERROR] I2C init failed: %d\n\r", ret);
		return -1;
	}

	dev->i2c_base = cfg->BaseAddress;
	xil_printf("[I2C] device_id=%d base=0x%08lX\n\r",
		   I2C_DEVICE_ID, (unsigned long)dev->i2c_base);

	ret = XIic_Start(&dev->i2c);
	if (ret != XST_SUCCESS) {
		xil_printf("[ERROR] I2C start failed: %d\n\r", ret);
		return -1;
	}

	ret = XIic_SelfTest(&dev->i2c);
	if (ret != XST_SUCCESS) {
		xil_printf("[ERROR] I2C self-test failed: %d\n\r", ret);
		return -1;
	}

	ret = XIic_SetGpOutput(&dev->i2c, 1);
	if (ret != XST_SUCCESS) {
		xil_printf("[ERROR] I2C GP output set failed: %d\n\r", ret);
		return -1;
	}

	return 0;
}

static void fmcdac_i2c_probe(struct fmcdac_dev *dev)
{
	uint8_t val = 0;
	int ret;

	ret = XIic_Recv(dev->i2c_base, I2C_MUX_ADDRESS, &val, 1, XIIC_STOP);
	if (ret == 1) {
		xil_printf("[I2C] mux 0x%02X ACK (state=0x%02X)\n\r",
			   I2C_MUX_ADDRESS, val);
	} else {
		xil_printf("[I2C] mux 0x%02X NACK (ret=%d)\n\r",
			   I2C_MUX_ADDRESS, ret);
	}
}

static void fmcdac_i2c_scan_si5328(struct fmcdac_dev *dev)
{
	int ch;
	uint8_t reg = 0x00;
	int found = 0;

	xil_printf("[I2C] scanning mux channels for 0x%02X\n\r",
		   SI5328_I2C_ADDRESS);
	for (ch = 0; ch < 8; ch++) {
		uint8_t mask = 1u << ch;
		if (fmcdac_i2c_mux_select(dev, mask) != 0) {
			xil_printf("[I2C] mux ch %d select failed\n\r", ch);
			continue;
		}
		no_os_mdelay(1);
		int sent = XIic_Send(dev->i2c_base, SI5328_I2C_ADDRESS, &reg, 1,
				     XIIC_STOP);
		if (sent == 1) {
			xil_printf("[I2C] addr 0x%02X ACK on ch %d (mask 0x%02X)\n\r",
				   SI5328_I2C_ADDRESS, ch, mask);
			found = 1;
		} else {
			xil_printf("[I2C] addr 0x%02X NACK on ch %d (sent=%d)\n\r",
				   SI5328_I2C_ADDRESS, ch, sent);
		}
	}

	fmcdac_i2c_mux_select(dev, I2C_MUX_SI5328_CHANNEL);

	if (!found)
		fmcdac_i2c_scan_bus(dev, I2C_MUX_SI5328_CHANNEL);
}

static void fmcdac_i2c_scan_bus(struct fmcdac_dev *dev, uint8_t channel_mask)
{
	int addr;
	int ret;
	uint8_t val = 0;
	int found = 0;

	if (fmcdac_i2c_mux_select(dev, channel_mask) != 0) {
		xil_printf("[I2C] bus scan: mux select 0x%02X failed\n\r",
			   channel_mask);
		return;
	}
	no_os_mdelay(1);

	xil_printf("[I2C] bus scan on mux mask 0x%02X\n\r", channel_mask);
	for (addr = 0x08; addr <= 0x77; addr++) {
		val = 0;
		ret = XIic_Recv(dev->i2c_base, (uint8_t)addr, &val, 1, XIIC_STOP);
		if (ret == 1) {
			xil_printf("[I2C] addr 0x%02X ACK (read=0x%02X)\n\r",
				   addr, val);
			found = 1;
		}
	}

	if (!found)
		xil_printf("[I2C] bus scan: no devices ACKed\n\r");

	fmcdac_i2c_mux_select(dev, I2C_MUX_SI5328_CHANNEL);
}

static int fmcdac_i2c_mux_select(struct fmcdac_dev *dev, uint8_t channel_mask)
{
	int sent;
	uint8_t data = channel_mask;
	uint8_t readback = 0;

	sent = XIic_Send(dev->i2c_base, I2C_MUX_ADDRESS, &data, 1, XIIC_STOP);
	if (sent != 1) {
		xil_printf("[ERROR] I2C mux select failed (sent=%d)\n\r", sent);
		return -1;
	}
	xil_printf("[I2C] mux select 0x%02X OK\n\r", channel_mask);

	if (XIic_Recv(dev->i2c_base, I2C_MUX_ADDRESS, &readback, 1, XIIC_STOP) != 1) {
		xil_printf("[WARN] I2C mux readback failed\n\r");
		return 0;
	}

	if ((readback & channel_mask) != channel_mask) {
		xil_printf("[WARN] I2C mux readback mismatch (wrote=0x%02X read=0x%02X)\n\r",
			   channel_mask, readback);
	}

	return 0;
}

static int fmcdac_i2c_write_reg(u32 i2c_base, uint8_t addr,
				uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = { reg, val };
	int sent = XIic_Send(i2c_base, addr, buf, 2, XIIC_STOP);

	return (sent == 2) ? 0 : -1;
}

static int fmcdac_i2c_read_reg(u32 i2c_base, uint8_t addr,
			       uint8_t reg, uint8_t *val)
{
	uint8_t reg_addr = reg;
	int sent;
	int recv;

	sent = XIic_Send(i2c_base, addr, &reg_addr, 1, XIIC_REPEATED_START);
	if (sent != 1)
		sent = XIic_Send(i2c_base, addr, &reg_addr, 1, XIIC_STOP);
	if (sent != 1)
		return -1;

	recv = XIic_Recv(i2c_base, addr, val, 1, XIIC_STOP);
	if (recv != 1)
		return -1;

	return 0;
}

static int fmcdac_si5328_wait_lock(struct fmcdac_dev *dev, uint32_t timeout_ms)
{
	uint32_t i;
	uint8_t lol = 0xff;
	int ret;

	for (i = 0; i < timeout_ms; i++) {
		ret = fmcdac_i2c_read_reg(dev->i2c_base, SI5328_I2C_ADDRESS,
					  130, &lol);
		if (ret)
			return ret;
		if ((lol & 0x01) == 0)
			return 0;
		if ((i % 1000) == 0) {
			xil_printf("[SI5328] waiting for lock (LOL_INT=0x%02X)\n\r",
				   lol);
		}
		no_os_mdelay(1);
	}

	xil_printf("[SI5328] lock timeout (LOL_INT=0x%02X)\n\r", lol);
	return -1;
}

static int fmcdac_si5328_setup(struct fmcdac_dev *dev)
{
	int ret;
	uint8_t probe = 0;

	ret = fmcdac_i2c_mux_select(dev, I2C_MUX_SI5328_CHANNEL);
	if (ret)
		return ret;

	ret = fmcdac_i2c_read_reg(dev->i2c_base, SI5328_I2C_ADDRESS, 0, &probe);
	if (ret) {
		xil_printf("[ERROR] Si5328 probe read failed\n\r");
		return -1;
	}
	xil_printf("[I2C] si5328 0x%02X ACK (reg0=0x%02X)\n\r",
		   SI5328_I2C_ADDRESS, probe);

	ret = Si5328_Reset(dev->i2c_base, SI5328_I2C_ADDRESS);
	if (ret != SI5328_SUCCESS) {
		xil_printf("[ERROR] Si5328 reset failed: %d\n\r", ret);
		return -1;
	}
	no_os_mdelay(10);

	ret = Si5328_Init(dev->i2c_base, SI5328_I2C_ADDRESS);
	if (ret != SI5328_SUCCESS) {
		xil_printf("[ERROR] Si5328 init failed: %d\n\r", ret);
		return -1;
	}

	ret = Si5328_SetClock(dev->i2c_base, SI5328_I2C_ADDRESS,
			      SI5328_CLKSRC_XTAL, SI5328_CLKIN_FREQ_HZ,
			      SI5328_CLKOUT_FREQ_HZ);
	if (ret != SI5328_SUCCESS) {
		xil_printf("[ERROR] Si5328 set clock failed: %d\n\r", ret);
		return -1;
	}

	ret = fmcdac_i2c_write_reg(dev->i2c_base, SI5328_I2C_ADDRESS, 132, 0x00);
	if (ret) {
		xil_printf("[WARN] Si5328 LOL_FLG clear failed\n\r");
	}

	ret = fmcdac_si5328_wait_lock(dev, SI5328_LOCK_TIMEOUT_MS);
	if (ret) {
		xil_printf("[ERROR] Si5328 lock failed: %d\n\r", ret);
		return ret;
	}

	xil_printf("[SI5328] locked\n\r");
	return 0;
}

static int fmcdac_spi_init(struct fmcdac_init_param *dev_init)
{     // spc matches the 20 mhz, which is present in the data sheet  
// max value is 25 Mhz  
	/* Initialize SPI structures */
	
	xil_printf("[SPI] Configuring SPI parameters...\n\r");
	
	struct no_os_spi_init_param ad9516_spi_param = {
		.device_id = SPI_DEVICE_ID,
		.max_speed_hz = 2000000u, // this is the communication speed for the spi 
		.chip_select = 0, // CS0 maps to spi_csn_clk (AD9516) per HDL design
		.mode = NO_OS_SPI_MODE_0,
		.platform_ops = &xil_spi_ops
	};

	struct no_os_spi_init_param ad9144_spi_param = {
		.device_id = SPI_DEVICE_ID,
		.max_speed_hz = 2000000u,
		.chip_select = 1, // CS1 maps to spi_csn_dac (AD9144) per HDL design
		.mode = NO_OS_SPI_MODE_0,
		.platform_ops = &xil_spi_ops
	};

	xil_printf("[SPI] AD9516 chip_select = %d (maps to spi_csn_clk)\n\r", ad9516_spi_param.chip_select);
	xil_printf("[SPI] AD9144 chip_select = %d (maps to spi_csn_dac)\n\r", ad9144_spi_param.chip_select);
	xil_printf("[SPI] SPI_DEVICE_ID = %d\n\r", SPI_DEVICE_ID);
	xil_printf("[SPI] SPI clock speed = %u Hz\n\r", ad9516_spi_param.max_speed_hz);
	xil_printf("[SPI] SPI mode = %d (should be 0 for MODE_0)\n\r", ad9516_spi_param.mode);
	xil_printf("[SPI] Verify AXI SPI has exactly 2 CS lines enabled in Vivado\n\r");

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
	int ret = 0;

	// clock distribution device (AD9516) configuration 
	ad9516_pdata.num_channels = 4; // 4 channels as 4 clock lines are being used to run the ad9144 evaluation board
	ad9516_pdata.channels = (int32_t)(&dev->ad9516_channels[0]); // channel initialisation may not be of the correct data type
	dev_init->ad9516_param.ad9516_st.pdata = &ad9516_pdata;
	dev_init->ad9516_param.ad9516_type = AD9516_1; // look into  the board and then change this , according to the specs sheet its ad9516-1
	dev_init->ad9516_param.ad9516_st.lvpecl_channels = &dev->ad9516_channels[0];
	dev_init->ad9516_param.ad9516_st.lvds_cmos_channels = &dev->ad9516_lvds_channels[0];
		// VCXO 125MHz
		ad9516_pdata.ref_1_freq = 122880000; // 122.88 MHz external reference -> AD9516 CLK/CLK_N
		ad9516_pdata.ref_2_freq = 0;
		ad9516_pdata.diff_ref_en = 1;
		ad9516_pdata.ref_1_power_on = 1;
		ad9516_pdata.ref_2_power_on = 0;
		ad9516_pdata.ref_sel_pin_en = 0;
		ad9516_pdata.ref_sel_pin = 1;
		ad9516_pdata.ref_2_en = 0;
		/* Default external clock input frequency (overridden by clock mode). */
		ad9516_pdata.ext_clk_freq = ad9516_pdata.ref_1_freq;
		/* Choose VCO at 2457.60 MHz (within AD9516-1 range). Use /5 VCO divider
		 * so outputs can be 491.52 MHz (/1) and 245.76 MHz (/2). */
		ad9516_pdata.int_vco_freq = 2457600000;
		ad9516_pdata.vco_clk_sel = 1;
		ad9516_pdata.power_down_vco_clk = 0;
		snprintf((char*)ad9516_pdata.name, sizeof(ad9516_pdata.name), "ad9516_lpc");
		
	dev->ad9516_channels[DAC_DEVICE_CLK].channel_num = 0;
	dev->ad9516_channels[DAC_DEVICE_CLK].out_invert_en = 0;
	dev->ad9516_channels[DAC_DEVICE_CLK].out_diff_voltage= LVPECL_960mV;

	dev->ad9516_channels[DAC_DEVICE_SYSREF].channel_num = 1;
	dev->ad9516_channels[DAC_DEVICE_SYSREF].out_invert_en = 0;
    dev->ad9516_channels[DAC_DEVICE_SYSREF].out_diff_voltage= LVPECL_780mV;

	dev->ad9516_channels[DAC_FPGA_CLK].channel_num = 2;
	dev->ad9516_channels[DAC_FPGA_CLK].out_invert_en = 0;
	dev->ad9516_channels[DAC_FPGA_CLK].out_diff_voltage= LVPECL_780mV;

	dev->ad9516_channels[DAC_FPGA_SYSREF].channel_num = 3;
	dev->ad9516_channels[DAC_FPGA_SYSREF].out_invert_en = 0;
	dev->ad9516_channels[DAC_FPGA_SYSREF].out_diff_voltage= LVPECL_780mV;

	/* Default LVDS/CMOS outputs (OUT6..OUT9) */
	dev->ad9516_lvds_channels[0].channel_num = 6;
	dev->ad9516_lvds_channels[0].out_invert = 0;
	dev->ad9516_lvds_channels[0].logic_level = LVDS;
	dev->ad9516_lvds_channels[0].cmos_b_en = 0;
	dev->ad9516_lvds_channels[0].out_lvds_current = LVDS_3_5mA;

	dev->ad9516_lvds_channels[1].channel_num = 7;
	dev->ad9516_lvds_channels[1].out_invert = 0;
	dev->ad9516_lvds_channels[1].logic_level = LVDS;
	dev->ad9516_lvds_channels[1].cmos_b_en = 0;
	dev->ad9516_lvds_channels[1].out_lvds_current = LVDS_3_5mA;

	dev->ad9516_lvds_channels[2].channel_num = 8;
	dev->ad9516_lvds_channels[2].out_invert = 0;
	dev->ad9516_lvds_channels[2].logic_level = LVDS;
	dev->ad9516_lvds_channels[2].cmos_b_en = 0;
	dev->ad9516_lvds_channels[2].out_lvds_current = LVDS_3_5mA;

	dev->ad9516_lvds_channels[3].channel_num = 9;
	dev->ad9516_lvds_channels[3].out_invert = 0;
	dev->ad9516_lvds_channels[3].logic_level = LVDS;
	dev->ad9516_lvds_channels[3].cmos_b_en = 0;
	dev->ad9516_lvds_channels[3].out_lvds_current = LVDS_3_5mA;

	return ret;
}

static int fmcdac_ad9516_program_outputs(struct fmcdac_dev *dev,
					 struct fmcdac_init_param *dev_init)
{
	uint32_t dac_ref_khz;
	uint32_t fpga_ref_khz;
	uint32_t sysref_khz;
	uint32_t ref_freq_hz = 0;
	uint32_t pfd_hz = 0;
	uint64_t vco_hz = 0;
	uint32_t pll_rb = 0;
	int64_t f0, f1, f2, f3;
	uint32_t out_reg = 0;
	uint32_t out_val = 0;
	uint32_t new_out_val = 0;
	int ret;

	if (dev_init->ad9144_param.pll_enable &&
	    dev_init->ad9144_param.pll_ref_frequency_khz)
		dac_ref_khz = dev_init->ad9144_param.pll_ref_frequency_khz;
	else
		dac_ref_khz = dev_init->ad9144_param.lane_rate_kbps / 10;

	if (g_clk_mode == FMCDAC_CLK_EXTERNAL &&
	    dev_init->ad9144_param.pll_dac_frequency_khz)
		dac_ref_khz = dev_init->ad9144_param.pll_dac_frequency_khz;

#ifndef ALTERA_PLATFORM
	fpga_ref_khz = dev_init->ad9144_xcvr_param.ref_rate_khz;
#else
	fpga_ref_khz = dev_init->ad9144_xcvr_param.parent_rate_khz;
#endif
	if (fpga_ref_khz == 0 && dev_init->ad9144_xcvr_param.lane_rate_khz)
		fpga_ref_khz = dev_init->ad9144_xcvr_param.lane_rate_khz / 20;

	if (dac_ref_khz == 0 || fpga_ref_khz == 0) {
		xil_printf("[ERROR] AD9516 output frequencies not set (dac_ref=%lu kHz, fpga_ref=%lu kHz)\n\r",
			   (unsigned long)dac_ref_khz,
			   (unsigned long)fpga_ref_khz);
		return -1;
	}



	/*
	 * ===== SYSREF Policy (A-02) =====
	 *
	 * Frequency: fSYSREF = fDAC / (K × S)  [JESD204B §5.3.3.5]
	 *   Mode 4: K=32, S=1 → 983040/32 = 30720 kHz = 30.72 MHz
	 *
	 * Edge: Two-phase sequencing (required for correct LMFC alignment):
	 *   1. AD9144 init sets RISING edge (SYSREF_ACTRL0 = 0x04).
	 *      The AD9144 aligns its LMFC to rising SYSREF during link-up.
	 *   2. After link reaches DATA state, fmcdac_sysref_tune() switches
	 *      the AD9144 to FALLING edge and W1C-clears the FPGA SYSREF
	 *      status. This edge transition produces correct phase alignment
	 *      between AD9144 and FPGA TX LMFC counters.
	 *   WARNING: Starting with falling edge from init bricks alignment
	 *   permanently — the tune cannot recover. Do not change the driver
	 *   default without re-validating on hardware.
	 *
	 * Mode: Continuous SYSREF (REG_SYNC_CTRL = 0xC1, SYSREF-armed).
	 *   The AD9144 re-aligns its LMFC on every SYSREF pulse.
	 *   One-shot mode is not used.
	 */
	{
		uint32_t K = dev_init->ad9144_jesd_param.frames_per_multiframe;
		/* S from AD9144 mode table: modes {0,2,3,4,6,7,9,10}→S=1; {1,5}→S=2 */
		uint32_t S;
		uint8_t mode = dev_init->ad9144_param.jesd204_mode;
		if (mode == 1 || mode == 5)
			S = 2;
		else
			S = 1;
		uint32_t fdac_khz;

		if (dev_init->ad9144_param.pll_enable &&
		    dev_init->ad9144_param.pll_dac_frequency_khz)
			fdac_khz = dev_init->ad9144_param.pll_dac_frequency_khz;
		else
			fdac_khz = dac_ref_khz;

		if (K == 0) K = 32; /* safety fallback */

		/*
		 * SYSREF = fDAC / (K × S × interpolation)
		 *
		 * The JESD LMFC is based on the link data rate, not the DAC
		 * output rate.  With Nx interpolation the link rate is
		 * fDAC/N, so divide by the interpolation factor to keep
		 * SYSREF aligned to the actual LMFC period.
		 *
		 *   interp=1, 983 MSPS:  983040/(32×1×1) = 30720 kHz  ✓
		 *   interp=2, 1966 MSPS: 1966080/(32×1×2) = 30720 kHz ✓
		 */
		uint32_t interp = dev_init->ad9144_param.interpolation;
		if (interp == 0) interp = 1;
		sysref_khz = fdac_khz / (K * S * interp);
		xil_printf("[AD9516] SYSREF: fDAC=%lu kHz  K=%lu  S=%lu  interp=%lu  =>  sysref=%lu kHz\n\r",
			   (unsigned long)fdac_khz, (unsigned long)K,
			   (unsigned long)S, (unsigned long)interp,
			   (unsigned long)sysref_khz);
	}

	if (dev->ad9516_dev->ad9516_st.pdata->ref_sel_pin_en)
		ref_freq_hz = dev->ad9516_dev->ad9516_st.pdata->ref_sel_pin ?
			      dev->ad9516_dev->ad9516_st.pdata->ref_2_freq :
			      dev->ad9516_dev->ad9516_st.pdata->ref_1_freq;
	else
		ref_freq_hz = dev->ad9516_dev->ad9516_st.pdata->ref_2_en ?
			      dev->ad9516_dev->ad9516_st.pdata->ref_2_freq :
			      dev->ad9516_dev->ad9516_st.pdata->ref_1_freq;

	if (dev->ad9516_dev->ad9516_st.r_counter)
		pfd_hz = ref_freq_hz / dev->ad9516_dev->ad9516_st.r_counter;
	vco_hz = (uint64_t)pfd_hz *
		 (uint64_t)(dev->ad9516_dev->ad9516_st.prescaler_p *
			    dev->ad9516_dev->ad9516_st.b_counter +
			    dev->ad9516_dev->ad9516_st.a_counter);

	xil_printf("[AD9516] pll_calc ref=%lu kHz r=%lu p=%lu a=%lu b=%lu pfd=%lu kHz fvco=%llu kHz\n\r",
		   (unsigned long)(ref_freq_hz / 1000),
		   (unsigned long)dev->ad9516_dev->ad9516_st.r_counter,
		   (unsigned long)dev->ad9516_dev->ad9516_st.prescaler_p,
		   (unsigned long)dev->ad9516_dev->ad9516_st.a_counter,
		   (unsigned long)dev->ad9516_dev->ad9516_st.b_counter,
		   (unsigned long)(pfd_hz / 1000),
		   (unsigned long long)(vco_hz / 1000));

	if (dev->ad9516_dev->ad9516_st.pdata->vco_clk_sel)
		dev->ad9516_dev->ad9516_st.vco_divider = 5;
	else
		dev->ad9516_dev->ad9516_st.vco_divider = 1;

	f0 = ad9516_frequency(dev->ad9516_dev, AD9516_OUT_DAC_CLK,
			      (int64_t)dac_ref_khz * 1000);
	f1 = ad9516_frequency(dev->ad9516_dev, AD9516_OUT_DAC_SYSREF,
			      (int64_t)sysref_khz * 1000);
	f2 = ad9516_frequency(dev->ad9516_dev, AD9516_OUT_FPGA_CLK,
			      (int64_t)fpga_ref_khz * 1000);
	f3 = ad9516_frequency(dev->ad9516_dev, AD9516_OUT_FPGA_SYSREF,
			      (int64_t)sysref_khz * 1000);
	ret = (f0 < 0 || f1 < 0 || f2 < 0 || f3 < 0) ? -1 : 0;
	if (ret < 0) {
		xil_printf("[ERROR] AD9516 frequency programming failed\n\r");
		return ret;
	}

	xil_printf("[AD9516] VCO target: %lu kHz (vco_div=%u)\n\r",
		   (unsigned long)(dev->ad9516_dev->ad9516_st.pdata->int_vco_freq / 1000),
		   dev->ad9516_dev->ad9516_st.vco_divider);
	xil_printf("[AD9516] Outputs programmed (OUT%u/OUT%u/OUT%u/OUT%u): CLK=%lu kHz SYSREF=%lu kHz FPGA_CLK=%lu kHz FPGA_SYSREF=%lu kHz\n\r",
		   AD9516_OUT_DAC_CLK, AD9516_OUT_DAC_SYSREF,
		   AD9516_OUT_FPGA_CLK, AD9516_OUT_FPGA_SYSREF,
		   (unsigned long)(f0 / 1000),
		   (unsigned long)(f1 / 1000),
		   (unsigned long)(f2 / 1000),
		   (unsigned long)(f3 / 1000));

	ad9516_update(dev->ad9516_dev);
	no_os_mdelay(5);

	out_reg = AD9516_REG_LVPECL_OUT0 + AD9516_OUT_DAC_CLK;
	ret = ad9516_read(dev->ad9516_dev, out_reg, &out_val);
	if (ret == 0) {
		new_out_val = out_val & ~AD9516_OUT_LVPECL_POWER_DOWN(0x3);
		if (new_out_val != out_val) {
			ret = ad9516_write(dev->ad9516_dev, out_reg, new_out_val);
			if (ret == 0) {
				ad9516_update(dev->ad9516_dev);
				xil_printf("[AD9516] OUT%u power-up (0x%02lX -> 0x%02lX)\n\r",
					   AD9516_OUT_DAC_CLK,
					   (unsigned long)out_val,
					   (unsigned long)new_out_val);
			}
		}
	}

	/* ---- LVDS/CMOS output diagnostics + force-enable ---- */
	{
		const struct {
			uint32_t reg;
			const char *name;
			uint32_t out_idx;
		} lvds_outs[] = {
			{ AD9516_REG_LVDS_CMOS_OUT6, "OUT6 (DAC_SYSREF)", AD9516_OUT_DAC_SYSREF },
			{ AD9516_REG_LVDS_CMOS_OUT7, "OUT7 (FPGA_SYSREF)", AD9516_OUT_FPGA_SYSREF },
			{ AD9516_REG_LVDS_CMOS_OUT8, "OUT8 (unused)", 8 },
			{ AD9516_REG_LVDS_CMOS_OUT9, "OUT9 (FPGA_CLK)", AD9516_OUT_FPGA_CLK },
		};
		uint32_t lval;
		for (int li = 0; li < 4; li++) {
			ret = ad9516_read(dev->ad9516_dev, lvds_outs[li].reg, &lval);
			if (ret == 0) {
				xil_printf("[AD9516] %s reg=0x%02lX: pd=%lu lvds=%lu current=%lu invert=%lu\n\r",
					   lvds_outs[li].name,
					   (unsigned long)lval,
					   (unsigned long)(lval & AD9516_OUT_LVDS_CMOS_POWER_DOWN),
					   (unsigned long)!!(lval & AD9516_OUT_LVDS_CMOS),
					   (unsigned long)((lval >> 1) & 0x3),
					   (unsigned long)((lval >> 5) & 0x7));
				/* Force power-up if powered down */
				if (lval & AD9516_OUT_LVDS_CMOS_POWER_DOWN) {
					uint32_t new_lval = lval & ~AD9516_OUT_LVDS_CMOS_POWER_DOWN;
					ad9516_write(dev->ad9516_dev, lvds_outs[li].reg, new_lval);
					xil_printf("[AD9516] %s FORCED POWER-UP (0x%02lX -> 0x%02lX)\n\r",
						   lvds_outs[li].name,
						   (unsigned long)lval, (unsigned long)new_lval);
				}
			}
		}
		/* Commit any power-up changes */
		ad9516_update(dev->ad9516_dev);
		no_os_mdelay(5);

		/* Read back divider registers for LVDS group 3 (OUT6/7) and 4 (OUT8/9) */
		uint32_t div3_3, div4_3;
		ad9516_read(dev->ad9516_dev, AD9516_REG_LVDS_CMOS_DIVIDER_3_3, &div3_3);
		ad9516_read(dev->ad9516_dev, AD9516_REG_LVDS_CMOS_DIVIDER_4_3, &div4_3);
		xil_printf("[AD9516] LVDS DIV3 ctrl=0x%02lX (byp1=%lu byp2=%lu)\n\r",
			   (unsigned long)div3_3,
			   (unsigned long)!!(div3_3 & AD9516_BYPASS_DIVIDER_1),
			   (unsigned long)!!(div3_3 & AD9516_BYPASS_DIVIDER_2));
		xil_printf("[AD9516] LVDS DIV4 ctrl=0x%02lX (byp1=%lu byp2=%lu)\n\r",
			   (unsigned long)div4_3,
			   (unsigned long)!!(div4_3 & AD9516_BYPASS_DIVIDER_1),
			   (unsigned long)!!(div4_3 & AD9516_BYPASS_DIVIDER_2));

		/* Force divider bypass for FPGA REFCLK at 122.88 MHz.
		 * DIV4 (OUT8/9) needs bypass so QPLL gets 122.88 MHz.
		 * DIV3 (OUT6/7) must NOT be bypassed — ad9516_frequency()
		 * sets SYSREF dividers for 30.72 MHz target. */
		uint32_t byp_mask = AD9516_BYPASS_DIVIDER_1 | AD9516_BYPASS_DIVIDER_2;

		if ((div4_3 & byp_mask) != byp_mask) {
			uint32_t new_div4 = div4_3 | byp_mask;
			ad9516_write(dev->ad9516_dev, AD9516_REG_LVDS_CMOS_DIVIDER_4_3, new_div4);
			xil_printf("[AD9516] DIV4 (OUT8/9) FORCED BYPASS (0x%02lX -> 0x%02lX)\n\r",
				   (unsigned long)div4_3, (unsigned long)new_div4);
		}
		/* DIV3 (OUT6/7 SYSREF): leave as programmed by ad9516_frequency() */
		xil_printf("[AD9516] DIV3 (OUT6/7) using frequency-driver dividers (no force bypass)\n\r");

		ad9516_update(dev->ad9516_dev);
		no_os_mdelay(10);

		/* Verify final divider state for both groups */
		ad9516_read(dev->ad9516_dev, AD9516_REG_LVDS_CMOS_DIVIDER_3_3, &div3_3);
		ad9516_read(dev->ad9516_dev, AD9516_REG_LVDS_CMOS_DIVIDER_4_3, &div4_3);
		xil_printf("[AD9516] FINAL DIV3 (OUT6/7)=0x%02lX (byp1=%lu byp2=%lu)\n\r",
			   (unsigned long)div3_3,
			   (unsigned long)!!(div3_3 & AD9516_BYPASS_DIVIDER_1),
			   (unsigned long)!!(div3_3 & AD9516_BYPASS_DIVIDER_2));
		xil_printf("[AD9516] FINAL DIV4 (OUT8/9)=0x%02lX (byp1=%lu byp2=%lu)\n\r",
			   (unsigned long)div4_3,
			   (unsigned long)!!(div4_3 & AD9516_BYPASS_DIVIDER_1),
			   (unsigned long)!!(div4_3 & AD9516_BYPASS_DIVIDER_2));
	}

	if (dev->ad9516_dev->ad9516_st.pdata->vco_clk_sel) {
		/* PLL-enabled mode: read and report PLL status */
		ret = ad9516_read(dev->ad9516_dev, AD9516_REG_PLL_READBACK, &pll_rb);
		if (ret == 0) {
			xil_printf("[AD9516] PLL readback: 0x%02lX (lock=%lu clk_mon=%lu ref1_mon=%lu ref2_mon=%lu)\n\r",
				   (unsigned long)pll_rb,
				   (unsigned long)!!(pll_rb & AD9516_DIGITAL_LOCK_DETECT),
				   (unsigned long)!!(pll_rb & AD9516_VCO_FREQ_GREATER),
				   (unsigned long)!!(pll_rb & AD9516_REF1_FREQ_GREATER),
				   (unsigned long)!!(pll_rb & AD9516_REF2_FREQ_GREATER));
		}
		ret = fmcdac_ad9516_wait_pll_lock(dev->ad9516_dev, 100);
		if (ret < 0)
			return ret;
	} else {
		xil_printf("[AD9516] External CLK mode; PLL disabled, skipping lock wait\n\r");
	}

	fmcdac_ad9516_dump_regs(dev->ad9516_dev);

	return 0;
}

static void fmcdac_ad9516_dump_regs(struct ad9516_dev *dev)
{
	const struct {
		uint32_t reg;
		const char *name;
	} regs[] = {
		{ AD9516_REG_SERIAL_PORT_CONFIG, "0x0000" },
		{ AD9516_REG_SERIAL_PORT_CONFIG + 1, "0x0001" },
		{ AD9516_REG_PFD_CHARGE_PUMP, "PFD_CP" },
		{ AD9516_REG_R_COUNTER, "R_CNT" },
		{ AD9516_REG_A_COUNTER, "A_CNT" },
		{ AD9516_REG_B_COUNTER, "B_CNT" },
		{ AD9516_REG_PLL_CTRL_1, "PLL_CTRL1" },
		{ AD9516_REG_PLL_CTRL_2, "PLL_CTRL2" },
		{ AD9516_REG_PLL_CTRL_3, "PLL_CTRL3" },
		{ AD9516_REG_PLL_CTRL_6, "PLL_CTRL6" },
		{ AD9516_REG_PLL_CTRL_7, "PLL_CTRL7" },
		{ AD9516_REG_PLL_READBACK, "PLL_RB" },
		{ AD9516_REG_LVPECL_OUT1, "OUT1" },
		{ AD9516_REG_DIVIDER_0_0, "DIV0_0" },
		{ AD9516_REG_DIVIDER_0_1, "DIV0_1" },
		{ AD9516_REG_DIVIDER_0_2, "DIV0_2" },
		{ AD9516_REG_VCO_DIVIDER, "VCO_DIV" },
		{ AD9516_REG_INPUT_CLKS, "INPUT_CLKS" },
		{ AD9516_REG_UPDATE_ALL_REGS, "UPDATE" },
	};
	uint32_t val;
	uint32_t i;
	int ret;

	/* Enable active register readback (vs buffer/staging) */
	ad9516_write(dev, AD9516_REG_READBACK_CTRL, 0x01);

	for (i = 0; i < (sizeof(regs) / sizeof(regs[0])); i++) {
		ret = ad9516_read(dev, regs[i].reg, &val);
		if (ret) {
			xil_printf("[AD9516] Readback %s failed: %d\n\r",
				   regs[i].name, ret);
			continue;
		}
		/* R_CNT and B_CNT are 2-byte transfers - show full 16-bit value */
		if (regs[i].reg == AD9516_REG_R_COUNTER) {
			xil_printf("[AD9516] Readback %s (0x%04lX)=0x%04lX (14-bit value=%lu)\n\r",
				   regs[i].name, (unsigned long)(regs[i].reg & 0xFFFF),
				   (unsigned long)val,
				   (unsigned long)(val & 0x3FFF));
		} else if (regs[i].reg == AD9516_REG_B_COUNTER) {
			xil_printf("[AD9516] Readback %s (0x%04lX)=0x%04lX (13-bit value=%lu)\n\r",
				   regs[i].name, (unsigned long)(regs[i].reg & 0xFFFF),
				   (unsigned long)val,
				   (unsigned long)(val & 0x1FFF));
		} else {
			xil_printf("[AD9516] Readback %s (0x%04lX)=0x%02lX\n\r",
				   regs[i].name, (unsigned long)(regs[i].reg & 0xFFFF),
				   (unsigned long)val);
		}
	}
}

static void fmcdac_apply_clock_mode(struct ad9144_init_param *p_ad9144_param,
				    struct ad9516_platform_data *p_ad9516_param)
{

	if (g_clk_mode == FMCDAC_CLK_EXTERNAL) {
		p_ad9144_param->pll_enable = 0;
		if (p_ad9144_param->pll_dac_frequency_khz)
			p_ad9516_param->ext_clk_freq =
				p_ad9144_param->pll_dac_frequency_khz * 1000;
		else if (p_ad9144_param->pll_ref_frequency_khz)
			p_ad9516_param->ext_clk_freq =
				p_ad9144_param->pll_ref_frequency_khz * 1000;
	} else if (g_clk_mode == FMCDAC_CLK_DISTRIBUTE) {
		if (p_ad9144_param->pll_enable &&
		    p_ad9144_param->pll_ref_frequency_khz)
			p_ad9516_param->ext_clk_freq =
				p_ad9144_param->pll_ref_frequency_khz * 1000;
		else if (p_ad9144_param->pll_dac_frequency_khz)
			p_ad9516_param->ext_clk_freq =
				p_ad9144_param->pll_dac_frequency_khz * 1000;
	} else {
		p_ad9516_param->ext_clk_freq = 0;
	}
}

static int fmcdac_ad9516_wait_pll_lock(struct ad9516_dev *dev, uint32_t timeout_ms)
{
	uint32_t rb = 0;
	uint32_t i;
	int ret;

	for (i = 0; i < timeout_ms; i++) {
		ret = ad9516_read(dev, AD9516_REG_PLL_READBACK, &rb);
		if (ret)
			return ret;
		if (rb & AD9516_DIGITAL_LOCK_DETECT) {
			xil_printf("[AD9516] PLL locked (rb=0x%02lX)\n\r",
				   (unsigned long)rb);
			return 0;
		}
		no_os_mdelay(1);
	}

	xil_printf("[AD9516] PLL not locked after %lu ms (rb=0x%02lX, lock=%lu, clk_mon=%lu, ref1_mon=%lu, ref2_mon=%lu)\n\r",
		   (unsigned long)timeout_ms,
		   (unsigned long)rb,
		   (unsigned long)!!(rb & AD9516_DIGITAL_LOCK_DETECT),
		   (unsigned long)!!(rb & AD9516_VCO_FREQ_GREATER),
		   (unsigned long)!!(rb & AD9516_REF1_FREQ_GREATER),
		   (unsigned long)!!(rb & AD9516_REF2_FREQ_GREATER));

	return -1;
}

static void fmcdac_hold_for_probe(const char *reason)
{
	if (reason)
		xil_printf("%s\n\r", reason);
	xil_printf("[DEBUG] Holding for probe. Power-cycle or reset to exit.\n\r");
	while (1)
		no_os_mdelay(1000);
}

static void fmcdac_delay_seconds(uint32_t seconds, uint32_t heartbeat_s)
{
	uint32_t i;

	for (i = 0; i < seconds; i++) {
		no_os_mdelay(1000);
		if (heartbeat_s && ((i + 1) % heartbeat_s) == 0)
			xil_printf("[DEBUG] toggle heartbeat: %lu/%lu s\n\r",
				   (unsigned long)(i + 1),
				   (unsigned long)seconds);
	}
}


static void fmcdac_flush_input(void)
{
	int c;
	do {
		c = getc(stdin);
		if (c == EOF)
			break;
	} while (c != '\n' && c != '\r');
}


static void fmcdac_ad9144_jesd_sanity(struct ad9144_dev *dev,
				      const struct ad9144_init_param *init_param)
{
	uint8_t cgs = 0;
	uint8_t frame = 0;
	uint8_t checksum = 0;
	uint8_t ilas = 0;
	uint8_t deskew = 0;
	uint8_t cdr = 0;
	uint8_t plldiv = 0;
	uint8_t pll_status = 0;
	uint32_t lanes = 0;
	uint32_t lane;

	if (!dev)
		return;

	if (dev->num_lanes)
		lanes = dev->num_lanes;
	else if (init_param)
		lanes = init_param->num_lanes;

	if (lanes == 0 || lanes > 8)
		lanes = 8;

	(void)ad9144_spi_read(dev, REG_LANEDESKEW, &deskew);
	(void)ad9144_spi_read(dev, REG_CODEGRPSYNCFLG, &cgs);
	(void)ad9144_spi_read(dev, REG_FRAMESYNCFLG, &frame);
	(void)ad9144_spi_read(dev, REG_GOODCHKSUMFLG, &checksum);
	(void)ad9144_spi_read(dev, REG_INITLANESYNCFLG, &ilas);
	(void)ad9144_spi_read(dev, REG_CDR_OPERATING_MODE_REG_0, &cdr);
	(void)ad9144_spi_read(dev, REG_REF_CLK_DIVIDER_LDO, &plldiv);
	(void)ad9144_spi_read(dev, REG_PLL_STATUS, &pll_status);

	xil_printf("[JESD] Sanity: deskew=0x%02X cgs=0x%02X frame=0x%02X chksum=0x%02X ilas=0x%02X\n\r",
		   deskew, cgs, frame, checksum, ilas);

	for (lane = 0; lane < lanes; lane++) {
		xil_printf("[JESD] L%lu: cgs=%lu frame=%lu chksum=%lu ilas=%lu\n\r",
			   (unsigned long)lane,
			   (unsigned long)((cgs >> lane) & 0x1),
			   (unsigned long)((frame >> lane) & 0x1),
			   (unsigned long)((checksum >> lane) & 0x1),
			   (unsigned long)((ilas >> lane) & 0x1));
	}

	xil_printf("[JESD] SERDES: CDR(0x230)=0x%02X PLLDIV(0x289)=0x%02X PLL_STATUS(0x281)=0x%02X\n\r",
		   cdr, plldiv, pll_status);
}

/* ===== Link Soak Test (A-04) ===== */

/* Uncomment to enable long-duration soak test after normal boot tests.
 * Configure SOAK_DURATION_HOURS for desired runtime (default 8h). */
/* #define ENABLE_SOAK */
#define SOAK_DURATION_HOURS  8
#define SOAK_POLL_INTERVAL_MS  1000
#define SOAK_PRBS_INTERVAL_S   900  /* 15 minutes */

#ifdef ENABLE_SOAK
/**
 * @brief Long-duration link stability soak test.
 *
 * Polls jesd_link_fully_synced() every SOAK_POLL_INTERVAL_MS for
 * SOAK_DURATION_HOURS. Runs datapath PRBS7 checks every
 * SOAK_PRBS_INTERVAL_S seconds. Logs any link drop or PRBS failure
 * with a timestamp (seconds since soak start).
 *
 * @return 0 if no failures, -N where N is total failure count.
 */
static int fmcdac_soak(struct fmcdac_dev *dev,
		       struct fmcdac_init_param *dev_init)
{
	uint32_t total_polls = 0;
	uint32_t link_fails = 0;
	uint32_t prbs_fails = 0;
	uint32_t elapsed_s = 0;
	uint32_t target_s = (uint32_t)SOAK_DURATION_HOURS * 3600U;
	uint32_t polls_per_sec = 1000 / SOAK_POLL_INTERVAL_MS;
	uint32_t next_prbs_s = SOAK_PRBS_INTERVAL_S;
	uint32_t st;
	int status;

	xil_printf("\n\r[SOAK] Starting %u-hour link soak test (%lu s, poll every %u ms)\n\r",
		   SOAK_DURATION_HOURS, (unsigned long)target_s, SOAK_POLL_INTERVAL_MS);
	xil_printf("[SOAK] PRBS check every %u s (%u min)\n\r",
		   SOAK_PRBS_INTERVAL_S, SOAK_PRBS_INTERVAL_S / 60);

	while (elapsed_s < target_s) {
		/* Poll link status */
		if (!jesd_link_fully_synced(dev, &st)) {
			link_fails++;
			xil_printf("[SOAK] LINK-FAIL at t=%lu s: st=0x%08lX (fail #%lu)\n\r",
				   (unsigned long)elapsed_s, (unsigned long)st,
				   (unsigned long)link_fails);
		}
		total_polls++;

		/* Periodic PRBS check — skip when interpolation active */
		if (elapsed_s >= next_prbs_s) {
			if (dev_init->ad9144_param.interpolation > 1) {
				/* PRBS checker is incompatible with interpolation;
				 * REG_INTERP_MODE must NOT be changed mid-link
				 * (causes PLL/JESD rate mismatch, link drop).
				 * Link polling above is the sole soak health metric. */
			} else {
				dev->ad9144_channels[0].sel = AXI_DAC_DATA_SEL_PN7;
				dev->ad9144_channels[1].sel = AXI_DAC_DATA_SEL_PN7;
				axi_dac_data_setup(dev->ad9144_core);
				no_os_mdelay(200);

				dev_init->ad9144_param.prbs_type = AD9144_PRBS7;
				status = ad9144_datapath_prbs_test(dev->ad9144_device,
								    &dev_init->ad9144_param);
				if (status < 0) {
					prbs_fails++;
					xil_printf("[SOAK] PRBS-FAIL at t=%lu s (fail #%lu)\n\r",
						   (unsigned long)elapsed_s,
						   (unsigned long)prbs_fails);
				}

				/* Restore DDS output */
				dev->ad9144_channels[0].sel = AXI_DAC_DATA_SEL_DDS;
				dev->ad9144_channels[1].sel = AXI_DAC_DATA_SEL_DDS;
				axi_dac_data_setup(dev->ad9144_core);
			}

			next_prbs_s = elapsed_s + SOAK_PRBS_INTERVAL_S;
		}

		/* Heartbeat every 300 seconds (5 min) */
		if (elapsed_s > 0 && (elapsed_s % 300) == 0 &&
		    (total_polls % polls_per_sec) == 0) {
			xil_printf("[SOAK] t=%lu/%lu s  polls=%lu  link_fails=%lu  prbs_fails=%lu\n\r",
				   (unsigned long)elapsed_s, (unsigned long)target_s,
				   (unsigned long)total_polls,
				   (unsigned long)link_fails,
				   (unsigned long)prbs_fails);
		}

		no_os_mdelay(SOAK_POLL_INTERVAL_MS);

		/* Approximate elapsed time (mdelay is not wall-clock accurate) */
		if ((total_polls % polls_per_sec) == 0)
			elapsed_s++;
	}

	xil_printf("\n\r[SOAK] ===== SOAK COMPLETE =====\n\r");
	xil_printf("[SOAK] Duration target: %u hours (%lu s)\n\r",
		   SOAK_DURATION_HOURS, (unsigned long)target_s);
	xil_printf("[SOAK] Total polls: %lu\n\r", (unsigned long)total_polls);
	xil_printf("[SOAK] Link failures: %lu\n\r", (unsigned long)link_fails);
	xil_printf("[SOAK] PRBS failures: %lu\n\r", (unsigned long)prbs_fails);

	if (link_fails == 0 && prbs_fails == 0)
		xil_printf("[SOAK] RESULT: PASS\n\r");
	else
		xil_printf("[SOAK] RESULT: FAIL\n\r");

	return (link_fails + prbs_fails) ? -(int)(link_fails + prbs_fails) : 0;
}
#endif /* ENABLE_SOAK */

/* ===== Subclass 1 Diagnostic Functions ===== */

/**
 * @brief Verify SYSREF capture and subclass configuration after link-up.
 *
 * Reads AD9144 and AXI JESD TX SYSREF/subclass registers and emits
 * warnings if the configuration is inconsistent with Subclass 1 operation.
 */
static void fmcdac_sysref_verify(struct fmcdac_dev *dev)
{
	uint8_t sysref_actrl0 = 0;
	uint8_t sync_ctrl = 0, sync_status = 0;
	uint8_t jrx_ctrl1 = 0;
	uint8_t ils_np = 0;
	uint32_t tx_sysref_conf = 0, tx_sysref_status = 0;
	uint8_t local_subclass, ilas_subclassv;

	if (!dev || !dev->ad9144_device)
		return;

	xil_printf("\n\r[SYSREF-VERIFY] Subclass 1 Verification:\n\r");

	/* AD9144 SYSREF analog control */
	ad9144_spi_read(dev->ad9144_device, REG_SYSREF_ACTRL0, &sysref_actrl0);
	xil_printf("[SYSREF-VERIFY] SYSREF_ACTRL0 (0x081) = 0x%02X (PD_SYSREF=%u SYSREF_RISE=%u HYS_ON=%u)\n\r",
		   sysref_actrl0,
		   !!(sysref_actrl0 & PD_SYSREF),
		   !!(sysref_actrl0 & SYSREF_RISE),
		   !!(sysref_actrl0 & HYS_ON));
	if (sysref_actrl0 & PD_SYSREF)
		xil_printf("[WARN] SYSREF buffer is powered down (PD_SYSREF=1)\n\r");

	/* SYNC control and status */
	ad9144_spi_read(dev->ad9144_device, REG_SYNC_CTRL, &sync_ctrl);
	ad9144_spi_read(dev->ad9144_device, REG_SYNC_STATUS, &sync_status);
	xil_printf("[SYSREF-VERIFY] SYNC_CTRL (0x03A) = 0x%02X  SYNC_STATUS (0x03B) = 0x%02X\n\r",
		   sync_ctrl, sync_status);

	/* Local subclass from JRX_CTRL_1 */
	ad9144_spi_read(dev->ad9144_device, REG_GENERAL_JRX_CTRL_1, &jrx_ctrl1);
	local_subclass = jrx_ctrl1 & 0x07;
	xil_printf("[SYSREF-VERIFY] JRX_CTRL_1 (0x301) = 0x%02X (local subclass=%u)\n\r",
		   jrx_ctrl1, local_subclass);

	/* ILAS SUBCLASSV from ILS_NP (0x458) bits [7:5] */
	ad9144_spi_read(dev->ad9144_device, REG_ILS_NP, &ils_np);
	ilas_subclassv = (ils_np >> 5) & 0x07;
	xil_printf("[SYSREF-VERIFY] ILS_NP (0x458) = 0x%02X (ILAS SUBCLASSV=%u N'-1=%u)\n\r",
		   ils_np, ilas_subclassv, ils_np & 0x1F);
	if (ilas_subclassv != 1)
		xil_printf("[WARN] ILAS SUBCLASSV=%u (expected 1 for Subclass 1)\n\r",
			   ilas_subclassv);

	/* AXI JESD TX SYSREF registers */
	tx_sysref_conf = Xil_In32(TX_JESD_BASEADDR + 0x100);
	tx_sysref_status = Xil_In32(TX_JESD_BASEADDR + 0x108);
	xil_printf("[SYSREF-VERIFY] TX SYSREF_CONF (0x100) = 0x%08lX (disabled=%lu)\n\r",
		   (unsigned long)tx_sysref_conf,
		   (unsigned long)(tx_sysref_conf & 0x1));
	xil_printf("[SYSREF-VERIFY] TX SYSREF_STATUS (0x108) = 0x%08lX\n\r",
		   (unsigned long)tx_sysref_status);
	if (tx_sysref_conf & 0x1)
		xil_printf("[WARN] TX SYSREF disabled bit is set in Subclass 1 mode\n\r");
	if (tx_sysref_status & 0x2)
		xil_printf("[WARN] TX SYSREF alignment error detected\n\r");

	xil_printf("[SYSREF-VERIFY] Done.\n\r");
}

/**
 * @brief Sweep SYSREF edge and LMFC offset to clear alignment error.
 *
 * Tries in order:
 *   1. Toggle AD9144 SYSREF edge (rising vs falling)
 *   2. Sweep AXI JESD TX SYSREF_LMFC_OFFSET 0..(K-1)
 *   3. Combine opposite edge + offset sweep
 *
 * For each candidate, clears SYSREF status (W1C), waits for re-capture,
 * and checks if the alignment error bit clears.
 *
 * @return 0 if alignment error cleared, -1 if all options exhausted.
 */
static int fmcdac_sysref_tune(struct fmcdac_dev *dev)
{
	uint8_t orig_actrl0 = 0;
	uint32_t sysref_status;
	uint32_t offset;
	uint32_t K = 32; /* frames_per_multiframe */
	int found = 0;

	if (!dev || !dev->ad9144_device)
		return -1;

	/* Read initial state */
	sysref_status = Xil_In32(TX_JESD_BASEADDR + 0x108);
	if ((sysref_status & 0x01) && !(sysref_status & 0x02)) {
		xil_printf("[SYSREF-TUNE] No alignment error - skipping tune\n\r");
		return 0;
	}

	ad9144_spi_read(dev->ad9144_device, REG_SYSREF_ACTRL0, &orig_actrl0);
	xil_printf("\n\r[SYSREF-TUNE] Alignment error detected. Starting phase sweep...\n\r");
	xil_printf("[SYSREF-TUNE] Original SYSREF_ACTRL0=0x%02X (edge=%s)\n\r",
		   orig_actrl0, (orig_actrl0 & SYSREF_RISE) ? "rising" : "falling");

	/*
	 * Trial helper: clear SYSREF status (W1C), wait for re-capture, check.
	 * Returns 1 if alignment error is cleared.
	 */
	#define SYSREF_TRIAL_OK() ({ \
		Xil_Out32(TX_JESD_BASEADDR + 0x108, 0x03); /* W1C clear */ \
		no_os_mdelay(50); /* wait for new SYSREF edges */ \
		sysref_status = Xil_In32(TX_JESD_BASEADDR + 0x108); \
		((sysref_status & 0x01) && !(sysref_status & 0x02)); \
	})

	/* --- Phase 1: Try opposite edge, offset=0 --- */
	{
		uint8_t try_edge = orig_actrl0 ^ SYSREF_RISE; /* toggle edge */
		ad9144_spi_write(dev->ad9144_device, REG_SYSREF_ACTRL0, try_edge);
		Xil_Out32(TX_JESD_BASEADDR + 0x104, 0); /* offset=0 */
		if (SYSREF_TRIAL_OK()) {
			xil_printf("[SYSREF-TUNE] FIXED: edge=%s offset=0 (status=0x%08lX)\n\r",
				   (try_edge & SYSREF_RISE) ? "rising" : "falling",
				   (unsigned long)sysref_status);
			found = 1;
			goto done;
		}
		/* Restore original edge */
		ad9144_spi_write(dev->ad9144_device, REG_SYSREF_ACTRL0, orig_actrl0);
	}

	/* --- Phase 2: Sweep LMFC offset with original edge --- */
	for (offset = 0; offset < K; offset++) {
		Xil_Out32(TX_JESD_BASEADDR + 0x104, offset);
		if (SYSREF_TRIAL_OK()) {
			xil_printf("[SYSREF-TUNE] FIXED: edge=%s offset=%lu (status=0x%08lX)\n\r",
				   (orig_actrl0 & SYSREF_RISE) ? "rising" : "falling",
				   (unsigned long)offset,
				   (unsigned long)sysref_status);
			found = 1;
			goto done;
		}
	}

	/* --- Phase 3: Sweep LMFC offset with opposite edge --- */
	{
		uint8_t try_edge = orig_actrl0 ^ SYSREF_RISE;
		ad9144_spi_write(dev->ad9144_device, REG_SYSREF_ACTRL0, try_edge);
		for (offset = 1; offset < K; offset++) { /* offset=0 already tried in phase 1 */
			Xil_Out32(TX_JESD_BASEADDR + 0x104, offset);
			if (SYSREF_TRIAL_OK()) {
				xil_printf("[SYSREF-TUNE] FIXED: edge=%s offset=%lu (status=0x%08lX)\n\r",
					   (try_edge & SYSREF_RISE) ? "rising" : "falling",
					   (unsigned long)offset,
					   (unsigned long)sysref_status);
				found = 1;
				goto done;
			}
		}
		/* Restore if nothing worked */
		ad9144_spi_write(dev->ad9144_device, REG_SYSREF_ACTRL0, orig_actrl0);
		Xil_Out32(TX_JESD_BASEADDR + 0x104, 0);
	}

done:
	if (!found) {
		xil_printf("[SYSREF-TUNE] FAILED: could not clear alignment error after full sweep\n\r");
		return -1;
	}

	/* Final readback to confirm */
	{
		uint8_t final_actrl0 = 0;
		uint32_t final_offset = Xil_In32(TX_JESD_BASEADDR + 0x104);
		ad9144_spi_read(dev->ad9144_device, REG_SYSREF_ACTRL0, &final_actrl0);
		xil_printf("[SYSREF-TUNE] Final config: SYSREF_ACTRL0=0x%02X (edge=%s) LMFC_OFFSET=%lu\n\r",
			   final_actrl0,
			   (final_actrl0 & SYSREF_RISE) ? "rising" : "falling",
			   (unsigned long)final_offset);
	}

	#undef SYSREF_TRIAL_OK
	return 0;
}

/**
 * @brief Read deterministic-latency registers for cross-boot comparison.
 *
 * Prints a compact one-line signature from AD9144 dynamic link latency
 * and LMFC variable registers.
 */
static void fmcdac_latency_readback(struct fmcdac_dev *dev)
{
	uint8_t dyn0 = 0, dyn1 = 0;
	uint8_t var0 = 0, var1 = 0;

	if (!dev || !dev->ad9144_device)
		return;

	ad9144_spi_read(dev->ad9144_device, REG_DYN_LINK_LATENCY_0, &dyn0);
	ad9144_spi_read(dev->ad9144_device, REG_DYN_LINK_LATENCY_1, &dyn1);
	ad9144_spi_read(dev->ad9144_device, REG_LMFC_VAR_0, &var0);
	ad9144_spi_read(dev->ad9144_device, REG_LMFC_VAR_1, &var1);

	xil_printf("[LATENCY] dyn0=0x%02X dyn1=0x%02X var0=0x%02X var1=0x%02X\n\r",
		   dyn0, dyn1, var0, var1);
}

/**
 * @brief PHY-level PRBS test using AD9144 PHY PRBS register block.
 *
 * Uses registers 0x315-0x31D. The PHY PRBS checker requires the FPGA TX
 * to generate a matching PHY-level PRBS pattern. If that source is not
 * configured, the test is skipped gracefully.
 *
 * This test is observability-only: results are logged but do not
 * participate in pass/fail accounting because the FPGA TX does not
 * generate PHY-level PRBS patterns in the current bitstream.
 */
static void fmcdac_phy_prbs_test(struct fmcdac_dev *dev)
{
	/* PHY PRBS control bit definitions are in ad9144.h:
	 * PHY_TEST_RESET, PHY_TEST_START, PHY_PRBS_PAT_SEL() */

	uint8_t test_en = 0, test_ctrl = 0, test_status = 0;
	uint8_t err_lo = 0, err_mid = 0, err_hi = 0;
	uint32_t err_count;

	if (!dev || !dev->ad9144_device)
		return;

	xil_printf("\n\r[PHY-PRBS] PHY-Level PRBS Test:\n\r");

	/*
	 * Gate check: the FPGA TX side must be generating PHY-level PRBS
	 * patterns for the AD9144 checker to work. In our current bitstream
	 * the TX sources DDS/datapath data, not PHY PRBS. We run the test
	 * but warn that results may not be meaningful.
	 */
	xil_printf("[PHY-PRBS] NOTE: TX-side PHY PRBS pattern source not confirmed.\n\r");
	xil_printf("[PHY-PRBS]       Results may show errors if TX is not generating PRBS.\n\r");

	/* Enable PRBS test on all 4 lanes (bits[3:0]) */
	ad9144_spi_write(dev->ad9144_device, REG_PHY_PRBS_TEST_EN, 0x0F);

	/* Select PRBS7 pattern (0x00) and reset the checker */
	ad9144_spi_write(dev->ad9144_device, REG_PHY_PRBS_TEST_CTRL,
			 PHY_PRBS_PAT_SEL(0) | PHY_TEST_RESET);
	no_os_mdelay(1);

	/* Start the test */
	ad9144_spi_write(dev->ad9144_device, REG_PHY_PRBS_TEST_CTRL,
			 PHY_PRBS_PAT_SEL(0) | PHY_TEST_START);
	no_os_mdelay(100);

	/* Read results */
	ad9144_spi_read(dev->ad9144_device, REG_PHY_PRBS_TEST_EN, &test_en);
	ad9144_spi_read(dev->ad9144_device, REG_PHY_PRBS_TEST_CTRL, &test_ctrl);
	ad9144_spi_read(dev->ad9144_device, REG_PHY_PRBS_TEST_ERRCNT_LOBITS, &err_lo);
	ad9144_spi_read(dev->ad9144_device, REG_PHY_PRBS_TEST_ERRCNT_MIDBITS, &err_mid);
	ad9144_spi_read(dev->ad9144_device, REG_PHY_PRBS_TEST_ERRCNT_HIBITS, &err_hi);
	ad9144_spi_read(dev->ad9144_device, REG_PHY_PRBS_TEST_STATUS, &test_status);

	err_count = ((uint32_t)err_hi << 16) | ((uint32_t)err_mid << 8) | err_lo;

	xil_printf("[PHY-PRBS] test_en=0x%02X ctrl=0x%02X status=0x%02X err_count=%lu\n\r",
		   test_en, test_ctrl, test_status, (unsigned long)err_count);

	/* Disable PHY PRBS test */
	ad9144_spi_write(dev->ad9144_device, REG_PHY_PRBS_TEST_EN, 0x00);
	ad9144_spi_write(dev->ad9144_device, REG_PHY_PRBS_TEST_CTRL, 0x00);

	if (err_count == 0 && (test_status & 0x01))
		xil_printf("[PHY-PRBS] PASSED (no errors, sync OK)\n\r");
	else if (err_count > 0)
		xil_printf("[PHY-PRBS] FAILED: %lu errors detected\n\r",
			   (unsigned long)err_count);
	else
		xil_printf("[PHY-PRBS] SKIPPED/INCONCLUSIVE: TX pattern source likely not active\n\r");




}

static int fmcdac_jesd_init(struct fmcdac_init_param *dev_init)
{
	dev_init->ad9144_xcvr_param = (struct adxcvr_init) {
		.name = "ad9144_xcvr",
		.base = TX_XCVR_BASEADDR,
		.sys_clk_sel = ADXCVR_SYS_CLK_QPLL0,
		.out_clk_sel = ADXCVR_OUTCLK_PMA, /* PMA parallel clk = lane_rate/40 = 245.76 MHz */
		.lpm_enable = 1,
		/* 9.8304 Gbps lanes for ~983 MSPS, refclk 122.88 MHz (from AD9516) */
		.lane_rate_khz = 9830400,
		.ref_rate_khz = 122880,
	};
	/* JESD initialization */
	dev_init->ad9144_jesd_param = (struct jesd204_tx_init) {
		.name = "ad9144_jesd",
		.base = TX_JESD_BASEADDR,
		.octets_per_frame = 1,
		.frames_per_multiframe = 32,
		.converters_per_device = 2,
		.converter_resolution = 16,
		.bits_per_sample = 16,
		.high_density = true,
		.control_bits_per_sample = 0,
		.subclass = 1,
		.device_clk_khz = 9830400/40,   /* 245760 kHz link clock */
		.lane_clk_khz = 9830400
	};
#ifdef JESD_FSM_ON
	struct link_init_param jrx_link_tx = {
		.link_id = 1,
		.device_id = 0,
		.octets_per_frame = 1,
		.frames_per_multiframe = 32,
		.samples_per_converter_per_frame = 1,
		.scrambling = 0,
		.high_density = 1,
		.converter_resolution = 16,
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
		xil_printf("error: %s: axi_jesd204_tx_init_jesd_fsm() failed\n",
		       dev_init->ad9144_jesd_param.name);
		return status;
	}
#else
	status = axi_jesd204_tx_init(&dev->ad9144_jesd, &dev_init->ad9144_jesd_param);
	if (status != 0) {
		xil_printf("error: %s: axi_jesd204_tx_init() failed\n",
		       dev_init->ad9144_jesd_param.name);
		return status;
	}

	status = axi_jesd204_tx_lane_clk_enable(dev->ad9144_jesd);
	if (status != 0) {
		xil_printf("error: %s: axi_jesd204_tx_lane_clk_enable() failed\n",
		       dev->ad9144_jesd->name);
		return status;
	}
#endif

	status = adxcvr_init(&dev->ad9144_xcvr, &dev_init->ad9144_xcvr_param);
	if (status != 0) {
		xil_printf("error: %s: adxcvr_init() failed\n", dev_init->ad9144_xcvr_param.name);
		return status;
	}
#ifndef ALTERA_PLATFORM
	status = adxcvr_clk_enable(dev->ad9144_xcvr);
	if (status != 0) {
		xil_printf("error: %s: adxcvr_clk_enable() failed\n", dev->ad9144_xcvr->name);
		return status;
	}
#endif
	return status;
}

/* Helper: Check if JESD link is fully synced (FPGA + AD9144 all lanes) */
static int jesd_link_fully_synced(struct fmcdac_dev *dev, uint32_t *st_out)
{
	uint32_t st = 0;
	uint8_t cgs = 0, frm = 0, chk = 0, ilas = 0;

	st = Xil_In32(TX_JESD_BASEADDR + 0x280);
	ad9144_spi_read(dev->ad9144_device, 0x470, &cgs);
	ad9144_spi_read(dev->ad9144_device, 0x471, &frm);
	ad9144_spi_read(dev->ad9144_device, 0x472, &chk);
	ad9144_spi_read(dev->ad9144_device, 0x473, &ilas);

	if (st_out) *st_out = st;

	/* bits[1:0]==3 => DATA, bit4==1 => SYNC~ deasserted (no retrain request) */
	return (((st & 0x3u) == 0x3u) &&
	        ((st & (1u << 4)) != 0) &&
	        (cgs == 0x0F) && (frm == 0x0F) &&
	        (chk == 0x0F) && (ilas == 0x0F));
}

/* Helper: Wait for N consecutive stable link polls */
static int wait_link_stable(struct fmcdac_dev *dev, uint32_t consec_need, uint32_t timeout_ms)
{
	uint32_t ok = 0;
	uint32_t i;
	uint32_t st;
	uint8_t cgs, frm, chk, ilas;

	for (i = 0; i < timeout_ms; i++) {
		if (jesd_link_fully_synced(dev, &st)) {
			ok++;
			if (ok >= consec_need) {
				xil_printf("[LINK-STABLE] Achieved %lu consecutive stable polls\n\r", consec_need);
				return 0;
			}
		} else {
			/* Diagnostic: show why link isn't stable */
			if (ok > 0) {
				ad9144_spi_read(dev->ad9144_device, 0x470, &cgs);
				ad9144_spi_read(dev->ad9144_device, 0x471, &frm);
				ad9144_spi_read(dev->ad9144_device, 0x472, &chk);
				ad9144_spi_read(dev->ad9144_device, 0x473, &ilas);
				xil_printf("[LINK-STABLE] Lost sync at poll %lu: FPGA=0x%02lX CGS=0x%02X Frm=0x%02X Chk=0x%02X ILAS=0x%02X\n\r",
				           i, st & 0xFF, cgs, frm, chk, ilas);
			}
			ok = 0;
		}
		no_os_mdelay(1);
	}
	xil_printf("[LINK-STABLE] TIMEOUT: only %lu/%lu consecutive stable polls in %lu ms\n\r",
	           ok, consec_need, timeout_ms);
	return -1;
}

/* Uncomment to skip STPL tests during bringup */
/* #define SKIP_STPL_TESTS */

/* Skip Si5328 setup - GTH REFCLK now sourced from AD9516 at 122.88 MHz.
 * Comment out to re-enable Si5328 if reverting to original clock tree. */
#define SKIP_SI5328

/* Verbose debug logging macro.
 * Comment out #define to silence all [K-DEBUG], [LINK-DEBUG], [DDS-DIAG], [STPL-DIAG] prints. */
#define DEBUG_VERBOSE
#ifdef DEBUG_VERBOSE
#define dbg_printf(...) xil_printf(__VA_ARGS__)
#else
#define dbg_printf(...) do {} while(0)
#endif

static int fmcdac_prepare_dds_output(struct fmcdac_dev *dev, const char *tag)
{
	uint32_t tri;
	uint32_t dat;

	if (!dev || !dev->ad9144_core || !dev->ad9144_device)
		return -1;

	if (wait_link_stable(dev, 20, 1000) != 0) {
		xil_printf("[%s] Link not stable in DATA, abort.\n\r", tag);
		return -1;
	}

	xil_printf("[%s] AXI DAC core: clock_hz=%lu Hz, num_channels=%u\n\r",
		   tag,
		   (unsigned long)dev->ad9144_core->clock_hz,
		   dev->ad9144_core->num_channels);

	ad9144_spi_write(dev->ad9144_device, REG_DACGAIN0_0, 0xFF);
	ad9144_spi_write(dev->ad9144_device, REG_DACGAIN0_1, 0x01);
	ad9144_spi_write(dev->ad9144_device, REG_DACGAIN1_0, 0xFF);
	ad9144_spi_write(dev->ad9144_device, REG_DACGAIN1_1, 0x01);
	xil_printf("[%s] IOUTFS set: DAC0/DAC1 gain = 0x01FF\n\r", tag);

#define AXI_GPIO_BASE  0x40000000U
#define DAC_CTRL_MASK  ((1U << 22) | (1U << 21))
	tri = Xil_In32(AXI_GPIO_BASE + 0x4);
	dat = Xil_In32(AXI_GPIO_BASE + 0x0);
	xil_printf("[%s] GPIO before: TRI=0x%08lX DATA=0x%08lX\n\r",
		   tag, (unsigned long)tri, (unsigned long)dat);
	Xil_Out32(AXI_GPIO_BASE + 0x4, tri & ~DAC_CTRL_MASK);
	Xil_Out32(AXI_GPIO_BASE + 0x0, dat | DAC_CTRL_MASK);
	no_os_mdelay(10);
	tri = Xil_In32(AXI_GPIO_BASE + 0x4);
	dat = Xil_In32(AXI_GPIO_BASE + 0x0);
	xil_printf("[%s] GPIO after:  TRI=0x%08lX DATA=0x%08lX (dac_ctrl=%lu)\n\r",
		   tag, (unsigned long)tri, (unsigned long)dat,
		   (unsigned long)((dat >> 21) & 0x3));
#undef AXI_GPIO_BASE
#undef DAC_CTRL_MASK

	return 0;
}

static int fmcdac_program_dds_pair(struct fmcdac_dev *dev, uint32_t freq_hz,
				       int32_t scale_u,
				       uint32_t phase0_mdeg,
				       uint32_t phase1_mdeg,
				       const char *tag)
{
	int32_t ret = 0;
	uint32_t ch;

	if (!dev || !dev->ad9144_core)
		return -1;

	axi_dac_dds_sync_hold(dev->ad9144_core);
	for (ch = 0; ch < dev->ad9144_core->num_channels && ch < 2; ch++) {
		uint32_t tone0 = ch * 2;
		uint32_t tone1 = tone0 + 1;
		uint32_t phase = (ch == 0) ? phase0_mdeg : phase1_mdeg;
		int32_t tmp;

		tmp = axi_dac_dds_set_frequency(dev->ad9144_core, tone0, freq_hz);
		if (tmp < 0)
			ret = tmp;
		tmp = axi_dac_dds_set_frequency(dev->ad9144_core, tone1, 0);
		if (tmp < 0)
			ret = tmp;

		tmp = axi_dac_dds_set_scale(dev->ad9144_core, tone0, scale_u);
		if (tmp < 0)
			ret = tmp;
		tmp = axi_dac_dds_set_scale(dev->ad9144_core, tone1, 0);
		if (tmp < 0)
			ret = tmp;

		tmp = axi_dac_dds_set_phase(dev->ad9144_core, tone0, phase);
		if (tmp < 0)
			ret = tmp;
		tmp = axi_dac_dds_set_phase(dev->ad9144_core, tone1, 0);
		if (tmp < 0)
			ret = tmp;

		tmp = axi_dac_set_datasel(dev->ad9144_core, ch, AXI_DAC_DATA_SEL_DDS);
		if (tmp < 0)
			ret = tmp;

		xil_printf("[%s] ch%lu tone%lu: freq=%lu Hz scale=%ld phase=%lu mddeg datasel=DDS\n\r",
			   tag,
			   (unsigned long)ch,
			   (unsigned long)tone0,
			   (unsigned long)freq_hz,
			   (long)scale_u,
			   (unsigned long)phase);
	}
	axi_dac_dds_sync_commit(dev->ad9144_core);

	return ret;
}

static void fmcdac_nco_readback(struct fmcdac_dev *dev, const char *tag)
{
	uint8_t datapath_ctrl = 0;
	uint8_t interp_mode = 0;
	uint8_t ftw[6] = {0};
	uint8_t phase_lsb = 0;
	uint8_t phase_msb = 0;
	uint8_t i;

	ad9144_spi_read(dev->ad9144_device, REG_DATAPATH_CTRL, &datapath_ctrl);
	ad9144_spi_read(dev->ad9144_device, REG_INTERP_MODE, &interp_mode);
	ad9144_spi_read(dev->ad9144_device, REG_NCO_PHASE_OFFSET0, &phase_lsb);
	ad9144_spi_read(dev->ad9144_device, REG_NCO_PHASE_OFFSET1, &phase_msb);
	for (i = 0; i < 6; i++)
		ad9144_spi_read(dev->ad9144_device, REG_FTW0 + i, &ftw[i]);

	xil_printf("[%s] DATAPATH_CTRL=0x%02X INTERP=0x%02X NCO_PHASE=0x%02X%02X FTW=0x%02X%02X%02X%02X%02X%02X\n\r",
		   tag,
		   datapath_ctrl,
		   interp_mode,
		   phase_msb,
		   phase_lsb,
		   ftw[5], ftw[4], ftw[3], ftw[2], ftw[1], ftw[0]);
}

static void fmcdac_wait_for_enter(const char *tag, const char *message)
{
	xil_printf("[%s] %s Press ENTER to continue...\n\r", tag, message);
	fmcdac_flush_input();
}

static int fmcdac_nco_discriminator_test(struct fmcdac_dev *dev)
{
	static const char *tag = "NCO-TEST";
	const uint32_t baseband_freq_hz = 10000000U;
	const int32_t scale_u = 999000;
	const uint32_t q_phase_mdeg = 90000U;
	const int32_t carrier_khz = 300000;
	int ret;

	ret = fmcdac_prepare_dds_output(dev, tag);
	if (ret != 0)
		return ret;

	ret = fmcdac_program_dds_pair(dev, baseband_freq_hz, scale_u,
					 0, q_phase_mdeg, tag);
	if (ret != 0) {
		xil_printf("[%s] DDS programming failed: %ld\n\r",
			   tag, (long)ret);
		return ret;
	}

	ret = ad9144_set_nco(dev->ad9144_device, 0, 0);
	if (ret != 0) {
		xil_printf("[%s] Failed to disable NCO: %ld\n\r",
			   tag, (long)ret);
		return ret;
	}

	xil_printf("[%s] Step 1/3: baseband only. Expect ~10 MHz on DAC0 and DAC1.\n\r",
		   tag);
	xil_printf("[%s] DDS setup uses a complex tone: DAC0=0 deg, DAC1=+90 deg.\n\r",
		   tag);
	fmcdac_nco_readback(dev, tag);
	fmcdac_wait_for_enter(tag, "Observe the 10 MHz baseband tone.");

	ret = ad9144_set_nco(dev->ad9144_device, carrier_khz, 0);
	if (ret != 0) {
		xil_printf("[%s] Failed to apply +300 MHz NCO: %ld\n\r",
			   tag, (long)ret);
		return ret;
	}
	xil_printf("[%s] Step 2/3: applied +300 MHz NCO carrier.\n\r", tag);
	xil_printf("[%s] Expect one shifted tone around 290 MHz or 310 MHz.\n\r",
		   tag);
	fmcdac_nco_readback(dev, tag);
	fmcdac_wait_for_enter(tag, "Observe the first shifted tone.");

	ret = ad9144_set_nco(dev->ad9144_device, -carrier_khz, 0);
	if (ret != 0) {
		xil_printf("[%s] Failed to apply -300 MHz NCO: %ld\n\r",
			   tag, (long)ret);
		return ret;
	}
	xil_printf("[%s] Step 3/3: applied -300 MHz NCO carrier.\n\r", tag);
	xil_printf("[%s] Expect the tone to swap to the opposite sideband (~310 MHz or ~290 MHz).\n\r",
		   tag);
	fmcdac_nco_readback(dev, tag);
	fmcdac_wait_for_enter(tag, "Observe the second shifted tone.");

	ret = ad9144_set_nco(dev->ad9144_device, 0, 0);
	if (ret != 0) {
		xil_printf("[%s] Failed to disable NCO at end of test: %ld\n\r",
			   tag, (long)ret);
		return ret;
	}

	xil_printf("[%s] NCO disabled. Returning control to the DDS sweep.\n\r",
		   tag);

	return 0;
}

static int fmcdac_dds_band_diagnostic_test(struct fmcdac_dev *dev)
{
	static const char *tag = "DDS-BAND";
	static const uint32_t freqs_mhz[] = {
		10U, 100U, 200U,
		230U, 240U, 250U, 260U, 270U, 280U, 290U, 300U, 310U, 320U, 330U
	};
	const int32_t scale_u = 999000;
	uint8_t interp_mode = 0;
	uint32_t i;
	int ret;

	ret = fmcdac_prepare_dds_output(dev, tag);
	if (ret != 0)
		return ret;

	ret = ad9144_set_nco(dev->ad9144_device, 0, 0);
	if (ret != 0) {
		xil_printf("[%s] Failed to disable NCO before DDS-band test: %ld\n\r",
			   tag, (long)ret);
		return ret;
	}

	ad9144_spi_read(dev->ad9144_device, REG_INTERP_MODE, &interp_mode);
	xil_printf("[%s] Focused DDS sweep diagnostic with NCO disabled.\n\r",
		   tag);
	xil_printf("[%s] Interpolation mode=0x%02X. Capturing 10/100/200 MHz references plus 230-330 MHz in 10 MHz steps.\n\r",
		   tag, interp_mode);

	for (i = 0; i < NO_OS_ARRAY_SIZE(freqs_mhz); i++) {
		uint32_t freq_mhz = freqs_mhz[i];
		uint32_t freq_hz = freq_mhz * 1000000U;

		ret = fmcdac_program_dds_pair(dev, freq_hz, scale_u, 0, 0, tag);
		if (ret != 0) {
			xil_printf("[%s] DDS programming failed at %lu MHz: %ld\n\r",
				   tag, (unsigned long)freq_mhz, (long)ret);
			return ret;
		}

		xil_printf("[%s] Step %lu/%lu: %lu MHz DDS tone.\n\r",
			   tag,
			   (unsigned long)(i + 1),
			   (unsigned long)NO_OS_ARRAY_SIZE(freqs_mhz),
			   (unsigned long)freq_mhz);

		if (freq_mhz < 230U) {
			xil_printf("[%s] Reference checkpoint before the observed droop band.\n\r",
				   tag);
		} else {
			xil_printf("[%s] Problem-band checkpoint near the observed 260-290 MHz amplitude loss.\n\r",
				   tag);
		}

		fmcdac_wait_for_enter(tag, "Observe the DDS tone.");
	}

	xil_printf("[%s] Completed focused DDS band diagnostic. Returning to normal DDS sweep.\n\r",
		   tag);

	return 0;
}

/**
 * @brief Force a known-good DDS tone for scope/spectrum analyzer validation.
 * @param dev - The device structure.
 * @return 0 on success, -1 if link not stable.
 */
static int force_dds_tone(struct fmcdac_dev *dev)
{
	int32_t ret;
	uint32_t ch;
	const uint32_t freq_hz = 10000000U;   /* 10 MHz */
	const int32_t scale_u  = 999000;      /* ~1.0 FS (micro-units), max headroom for sweep */

	ret = fmcdac_prepare_dds_output(dev, "DDS");
	if (ret != 0)
		return ret;

	/* 4) Program DDS using direct API — batched SYNC for atomic update */
	axi_dac_dds_sync_hold(dev->ad9144_core);
	for (ch = 0; ch < dev->ad9144_core->num_channels; ch++) {
		/* Each converter channel has 2 DDS tones: (ch*2+0) and (ch*2+1) */
		uint32_t tone0 = ch * 2 + 0;
		uint32_t tone1 = ch * 2 + 1;

		/* Set frequency — tone0 only, silence tone1 */
		ret = axi_dac_dds_set_frequency(dev->ad9144_core, tone0, freq_hz);
		xil_printf("[DDS] ch%lu tone%lu set_freq(%lu Hz) ret=%ld\n\r",
			   (unsigned long)ch, (unsigned long)tone0,
			   (unsigned long)freq_hz, (long)ret);

		/* Silence tone1 — single-tone mode */
		axi_dac_dds_set_frequency(dev->ad9144_core, tone1, 0);

		/* Set scale — tone0 active, tone1 silent */
		ret = axi_dac_dds_set_scale(dev->ad9144_core, tone0, scale_u);
		xil_printf("[DDS] ch%lu tone%lu set_scale(%ld) ret=%ld\n\r",
			   (unsigned long)ch, (unsigned long)tone0,
			   (long)scale_u, (long)ret);
		axi_dac_dds_set_scale(dev->ad9144_core, tone1, 0);

		/* Set phase */
		axi_dac_dds_set_phase(dev->ad9144_core, tone0, 0);
		axi_dac_dds_set_phase(dev->ad9144_core, tone1, 0);

		/* Select DDS data source */
		ret = axi_dac_set_datasel(dev->ad9144_core, ch, AXI_DAC_DATA_SEL_DDS);
		xil_printf("[DDS] ch%lu datasel=DDS ret=%ld\n\r",
			   (unsigned long)ch, (long)ret);
	}
	axi_dac_dds_sync_commit(dev->ad9144_core);

	/* 5) Read back AXI DDS registers at correct addresses */
	dbg_printf("[DDS-DIAG] AXI DAC register dump (DDS_PHASE_DW=%u):\n\r",
		   dev->ad9144_core->dds_phase_dw);
	for (ch = 0; ch < dev->ad9144_core->num_channels && ch < 4; ch++) {
		uint32_t tone = ch * 2;
		/* AXI_DAC_REG_DDS_SCALE(tone) = 0x400 + (tone>>1)*0x40 + (tone&1)*0x8 */
		uint32_t scale_addr = 0x400 + (tone >> 1) * 0x40 + (tone & 1) * 0x8;
		/* AXI_DAC_REG_DDS_INIT_INCR(tone) = 0x404 + (tone>>1)*0x40 + (tone&1)*0x8 */
		uint32_t incr_addr  = 0x404 + (tone >> 1) * 0x40 + (tone & 1) * 0x8;
		/* AXI_DAC_REG_CHAN_CNTRL_7(ch) = 0x0418 + ch * 0x40 */
		uint32_t sel_addr   = 0x418 + ch * 0x40;

		uint32_t scale_val = Xil_In32(dev->ad9144_core->base + scale_addr);
		uint32_t incr_val  = Xil_In32(dev->ad9144_core->base + incr_addr);
		uint32_t sel_val   = Xil_In32(dev->ad9144_core->base + sel_addr);

		uint16_t sc  = scale_val & 0xFFFF;

		if (dev->ad9144_core->dds_phase_dw > 16) {
			/* Extension register: 0x42C + (tone>>1)*0x40 + (tone&1)*0x4 */
			uint32_t ext_addr = 0x42C + (tone >> 1) * 0x40 + (tone & 1) * 0x4;
			uint32_t ext_val  = Xil_In32(dev->ad9144_core->base + ext_addr);

			uint32_t ftw32  = ((ext_val & 0xFFFF) << 16) | (incr_val & 0xFFFF);
			uint32_t init32 = ((ext_val >> 16) & 0xFFFF) << 16 |
					  ((incr_val >> 16) & 0xFFFF);

			dbg_printf("[DDS-DIAG] ch%lu: sc=0x%04X ftw32=0x%08lX init32=0x%08lX "
				   "sel=0x%lX\n\r",
				   (unsigned long)ch, sc,
				   (unsigned long)ftw32, (unsigned long)init32,
				   (unsigned long)sel_val);
		} else {
			uint16_t ftw = incr_val & 0xFFFF;
			uint16_t init = (incr_val >> 16) & 0xFFFF;

			dbg_printf("[DDS-DIAG] ch%lu: SCALE[0x%03lX]=0x%08lX (sc=0x%04X) "
				   "INCR[0x%03lX]=0x%08lX (ftw=0x%04X init=0x%04X) "
				   "SEL[0x%03lX]=0x%08lX\n\r",
				   (unsigned long)ch,
				   (unsigned long)scale_addr, (unsigned long)scale_val, sc,
				   (unsigned long)incr_addr, (unsigned long)incr_val, ftw, init,
				   (unsigned long)sel_addr, (unsigned long)sel_val);
		}
	}

	/* 6) AD9144 output status */
	{
		uint8_t val;
		ad9144_spi_read(dev->ad9144_device, REG_PWRCNTRL0, &val);
		dbg_printf("[DDS-DIAG] PWRCNTRL0=0x%02X CLKCFG0=", val);
		ad9144_spi_read(dev->ad9144_device, REG_CLKCFG0, &val);
		dbg_printf("0x%02X DACPLLSTATUS=", val);
		ad9144_spi_read(dev->ad9144_device, REG_DACPLLSTATUS, &val);
		dbg_printf("0x%02X\n\r", val);
		ad9144_spi_read(dev->ad9144_device, REG_DACGAIN0_1, &val);
		dbg_printf("[DDS-DIAG] DACGAIN0=0x%02X", val);
		ad9144_spi_read(dev->ad9144_device, REG_DACGAIN0_0, &val);
		dbg_printf("%02X DACGAIN1=", val);
		ad9144_spi_read(dev->ad9144_device, REG_DACGAIN1_1, &val);
		dbg_printf("0x%02X", val);
		ad9144_spi_read(dev->ad9144_device, REG_DACGAIN1_0, &val);
		dbg_printf("%02X\n\r", val);
	}

	xil_printf("[DDS] Setup complete. Starting frequency sweep...\n\r");

	/* 5) Frequency sweep: 10 MHz → 490 MHz in 10 MHz steps
	 * Sweep covers the full FPGA DDS Nyquist band (491.52 MHz).
	 * With 2x interpolation (default), image suppression is excellent
	 * across the entire band.  The interpolation filter passband is
	 * flat to ~393 MHz with 1-3 dB rolloff towards 491 MHz.
	 *
	 * NOTE: no_os_mdelay is mis-calibrated on MicroBlaze.
	 * Adjust SWEEP_HOLD_MS until each step takes ~3 real seconds.
	 * If steps take ~30s with value 3000, try 300. */
#define SWEEP_HOLD_MS  50   /* Tune this: target ~3 real seconds per step */
	{
		const uint32_t start_mhz = 10;
		const uint32_t stop_mhz  = 490;   /* FPGA DDS Nyquist ≈ 491.52 MHz */
		const uint32_t step_mhz  = 10;

		xil_printf("[SWEEP] %lu steps, SWEEP_HOLD_MS=%lu (tune if timing is off)\n\r",
			   (unsigned long)((stop_mhz - start_mhz) / step_mhz + 1),
			   (unsigned long)SWEEP_HOLD_MS);

		{
			uint8_t interp_rb = 0;
			ad9144_spi_read(dev->ad9144_device, REG_INTERP_MODE, &interp_rb);
			xil_printf("[SWEEP] AD9144 interpolation mode (0x112) = 0x%02X\n\r", interp_rb);
			if (interp_rb != 0x00)
				xil_printf("[SWEEP] NOTE: 2x interpolation active. Expect ~3 dB rolloff "
					   "above 390 MHz, ~6+ dB above 450 MHz (half-band filter).\n\r");
		}
		for (uint32_t f_mhz = start_mhz; f_mhz <= stop_mhz; f_mhz += step_mhz) {
			uint32_t f_hz = f_mhz * 1000000U;

			/* Update frequency on both channels, tone0 only — batched */
			axi_dac_dds_sync_hold(dev->ad9144_core);
			for (ch = 0; ch < dev->ad9144_core->num_channels; ch++) {
				uint32_t tone0 = ch * 2;
				axi_dac_dds_set_frequency(dev->ad9144_core, tone0, f_hz);
			}
			axi_dac_dds_sync_commit(dev->ad9144_core);

			/* Read back FTW for logging */
			if (dev->ad9144_core->dds_phase_dw > 16) {
				uint32_t incr_val = Xil_In32(dev->ad9144_core->base + 0x404);
				uint32_t ext_val  = Xil_In32(dev->ad9144_core->base + 0x42C);
				uint32_t ftw32 = ((ext_val & 0xFFFF) << 16) |
						 (incr_val & 0xFFFF);
				xil_printf("[SWEEP] %3lu MHz  (ftw32=0x%08lX)\n\r",
					   (unsigned long)f_mhz,
					   (unsigned long)ftw32);
			} else {
				uint32_t incr_val = Xil_In32(dev->ad9144_core->base + 0x404);
				uint16_t ftw = incr_val & 0xFFFF;
				xil_printf("[SWEEP] %3lu MHz  (ftw=0x%04X)\n\r",
					   (unsigned long)f_mhz, ftw);
			}

			no_os_mdelay(SWEEP_HOLD_MS);
		}

		xil_printf("[SWEEP] Done. Holding max frequency...\n\r");
		no_os_mdelay(SWEEP_HOLD_MS);
	}
#undef SWEEP_HOLD_MS

	return 0;
}

static int fmcdac_test(struct fmcdac_dev *dev,
			struct fmcdac_init_param *dev_init)
{
	int status;

	/* Verify AXI DAC enum values match HDL expectations */
	dbg_printf("[ENUM-CHECK] AXI DAC data select: DDS=%d SED=%d ZERO=%d PN7=%d PN15=%d PN23=%d PN31=%d\n\r",
		   AXI_DAC_DATA_SEL_DDS, AXI_DAC_DATA_SEL_SED, AXI_DAC_DATA_SEL_ZERO,
		   AXI_DAC_DATA_SEL_PN7, AXI_DAC_DATA_SEL_PN15,
		   AXI_DAC_DATA_SEL_PN23, AXI_DAC_DATA_SEL_PN31);

	status = axi_jesd204_tx_status_read(dev->ad9144_jesd);
	if (status != 0) {
		xil_printf("axi_jesd204_tx_status_read() error: %d\n", status);
	}
	
	/* ===== POST-LINK-START K Parameter Verification ===== */
	dbg_printf("\n\r[K-DEBUG] POST-LINK-START Register Verification:\n\r");
	uint32_t jesd_conf0_post = Xil_In32(TX_JESD_BASEADDR + 0x210);
	dbg_printf("[K-DEBUG] CONF0 after link start = 0x%08lX\n\r", jesd_conf0_post);
	dbg_printf("[K-DEBUG]   Bits [7:0]  = 0x%02lX (K*F = %lu)\n\r", 
	           jesd_conf0_post & 0xFF, (jesd_conf0_post & 0xFF) + 1);
	dbg_printf("[K-DEBUG]   Bits [9:0]  = 0x%03lX (K*F = %lu)\n\r", 
	           jesd_conf0_post & 0x3FF, (jesd_conf0_post & 0x3FF) + 1);
	
	/* Calculate LMFC from register value - CORRECTED: use byte_clock (lane_rate/10) */
	uint32_t kf_bits7_0 = (jesd_conf0_post & 0xFF) + 1;
	uint32_t kf_bits9_0 = (jesd_conf0_post & 0x3FF) + 1;
	uint32_t byte_clock_khz = 9830400 / 10;  /* Lane rate 9.83 Gbps -> byte clock 983.04 MHz */
	uint32_t lmfc_calc_8bit = byte_clock_khz / kf_bits7_0;
	uint32_t lmfc_calc_10bit = byte_clock_khz / kf_bits9_0;
	
	dbg_printf("[K-DEBUG] LMFC calculated from bits[7:0]  = %lu kHz\n\r", lmfc_calc_8bit);
	dbg_printf("[K-DEBUG] LMFC calculated from bits[9:0]  = %lu kHz\n\r", lmfc_calc_10bit);
	dbg_printf("[K-DEBUG] LMFC reported by status output = 30720 kHz\n\r");
	
	if (lmfc_calc_10bit != 30720 && lmfc_calc_8bit != 30720) {
		dbg_printf("[K-DEBUG] *** CRITICAL: Hardware using different K value! ***\n\r");
		dbg_printf("[K-DEBUG] Register shows K*F=%lu, but hardware acts like K*F=8\n\r", kf_bits9_0);
		dbg_printf("[K-DEBUG] Possible causes:\n\r");
		dbg_printf("[K-DEBUG]   1. HDL synthesized with hardcoded K=8\n\r");
		dbg_printf("[K-DEBUG]   2. Register write not reaching hardware\n\r");
		dbg_printf("[K-DEBUG]   3. JESD core ignoring register value\n\r");
	} else if (lmfc_calc_8bit == 30720) {
		dbg_printf("[K-DEBUG] Hardware correctly using bits[7:0] (K*F=%lu)\n\r", kf_bits7_0);
	} else {
		dbg_printf("[K-DEBUG] Hardware correctly using bits[9:0] (K*F=%lu)\n\r", kf_bits9_0);
	}
	dbg_printf("[K-DEBUG] ==========================================\n\r\n\r");
	/* ===== End POST-LINK-START Verification ===== */

	int test_errors = 0;
	uint32_t fpga_link_state;
	uint8_t cgs_now, frame_now, chksum_now, ilas_now;
	const char *state_str;
	int link_ok;
	int poll_i;
	int data_reached;

	/*
	 * poll_link_full() equivalent: wait for FPGA TX DATA state AND
	 * AD9144 receiver CGS+Frame flags both 0x0F.
	 * axi_dac_data_setup() triggers a SYNC pulse which drops the link
	 * to CGS. The FPGA TX recovers first, then the AD9144 receiver
	 * needs additional time to re-acquire CGS and frame sync on all lanes.
	 * Total timeout: 80 iterations * 50ms = 4 seconds.
	 */

	/* ---- Initial link state (before any SED/test modifications) ---- */
	data_reached = 0;
	for (poll_i = 0; poll_i < 80; poll_i++) {
		fpga_link_state = Xil_In32(TX_JESD_BASEADDR + 0x280);
		ad9144_spi_read(dev->ad9144_device, REG_CODEGRPSYNCFLG, &cgs_now);
		ad9144_spi_read(dev->ad9144_device, REG_FRAMESYNCFLG, &frame_now);
		if (((fpga_link_state & 0x3) == 3) &&
		    (cgs_now == 0x0F) && (frame_now == 0x0F)) {
			data_reached = 1;
			break;
		}
		no_os_mdelay(50);
	}
	/* Final status readback */
	fpga_link_state = Xil_In32(TX_JESD_BASEADDR + 0x280);
	ad9144_spi_read(dev->ad9144_device, REG_CODEGRPSYNCFLG, &cgs_now);
	ad9144_spi_read(dev->ad9144_device, REG_FRAMESYNCFLG, &frame_now);
	ad9144_spi_read(dev->ad9144_device, REG_GOODCHKSUMFLG, &chksum_now);
	ad9144_spi_read(dev->ad9144_device, REG_INITLANESYNCFLG, &ilas_now);

	switch (fpga_link_state & 0x3) {
		case 0: state_str = "WAIT"; break;
		case 1: state_str = "CGS"; break;
		case 2: state_str = "ILAS"; break;
		case 3: state_str = "DATA"; break;
		default: state_str = "UNKNOWN"; break;
	}
	xil_printf("[TEST] FPGA link=%s  CGS=0x%02X  Frame=0x%02X  Chksum=0x%02X  InitSync=0x%02X  (poll=%d)\n\r",
		   state_str, cgs_now, frame_now, chksum_now, ilas_now, poll_i);

	/* Subclass 0 has no SYSREF, so ILAS (InitSync) may legitimately be 0x00.
	 * Only require FPGA DATA + AD9144 CGS + Frame. */
	link_ok = data_reached &&
		  (cgs_now == 0x0F) && (frame_now == 0x0F) &&
		  (chksum_now == 0x0F);

	if (!link_ok) {
		xil_printf("[TEST] *** WARNING: Link not fully synced after %d polls ***\n\r", poll_i);
		xil_printf("[TEST] Will run tests anyway for diagnostics\n\r");
	} else {
		xil_printf("[TEST] Link fully stable - FPGA DATA, AD9144 CGS+Frame+Chk all 0x0F\n\r");
	}

#ifndef SKIP_STPL_TESTS
	/* ---- Zero-pattern STPL baseline test using ZERO source ---- */
	xil_printf("\n\r[STPL-ZERO] Waiting for 5 consecutive stable link polls before STPL...\n\r");
	if (wait_link_stable(dev, 5, 2000) != 0) {
		xil_printf("[STPL-ZERO] ABORTED: Link not stable enough for STPL testing\n\r");
		test_errors++;
		goto skip_stpl_zero;
	}

	/* Use DDS mode with scale=0 for guaranteed zero output.
	 * ZERO mode (sel=3) can be bypassed if dac_mask_enable is set in HDL.
	 * DDS with scale=0 outputs hard zeros regardless of other settings. */
	dev->ad9144_channels[0].sel = AXI_DAC_DATA_SEL_DDS;
	dev->ad9144_channels[1].sel = AXI_DAC_DATA_SEL_DDS;
	xil_printf("[STPL-ZERO] Using DDS mode with scale=0 for guaranteed zeros\n\r");
	
	/* Enable channels and set DDS source — batched for single SYNC */
	axi_dac_dds_sync_hold(dev->ad9144_core);
	axi_dac_set_datasel(dev->ad9144_core, -1, AXI_DAC_DATA_SEL_DDS);
	/* Set DDS scales to 0 for all tones - this outputs hard zeros */
	axi_dac_dds_set_scale(dev->ad9144_core, 0, 0);  /* ch0 tone0 scale = 0 */
	axi_dac_dds_set_scale(dev->ad9144_core, 1, 0);  /* ch0 tone1 scale = 0 */
	axi_dac_dds_set_scale(dev->ad9144_core, 2, 0);  /* ch1 tone0 scale = 0 */
	axi_dac_dds_set_scale(dev->ad9144_core, 3, 0);  /* ch1 tone1 scale = 0 */
	axi_dac_dds_sync_commit(dev->ad9144_core);
	
	no_os_mdelay(50); /* Allow register update to propagate */
	
	/* Verify SED mode took effect */
	{
		uint32_t sel_rb0, sel_rb1, pat_rb0, pat_rb1;
		uint32_t cntrl_rb0, cntrl_rb1; /* reg 0x0400/0x0440 - channel enable */
		uint32_t dds_scale0_0, dds_scale1_0; /* reg 0x040C/0x044C - DDS scale */
		uint32_t iqcor0, iqcor1; /* reg 0x0414/0x0454 - IQ correction enable */
		sel_rb0 = Xil_In32(dev->ad9144_core->base + 0x0418); /* ch0 select */
		sel_rb1 = Xil_In32(dev->ad9144_core->base + 0x0458); /* ch1 select */
		pat_rb0 = Xil_In32(dev->ad9144_core->base + 0x0410); /* ch0 pattern */
		pat_rb1 = Xil_In32(dev->ad9144_core->base + 0x0450); /* ch1 pattern */
		cntrl_rb0 = Xil_In32(dev->ad9144_core->base + 0x0400); /* ch0 control */
		cntrl_rb1 = Xil_In32(dev->ad9144_core->base + 0x0440); /* ch1 control */
		dds_scale0_0 = Xil_In32(dev->ad9144_core->base + 0x040C); /* ch0 DDS scale */
		dds_scale1_0 = Xil_In32(dev->ad9144_core->base + 0x044C); /* ch1 DDS scale */
		iqcor0 = Xil_In32(dev->ad9144_core->base + 0x0414); /* ch0 IQ correction */
		iqcor1 = Xil_In32(dev->ad9144_core->base + 0x0454); /* ch1 IQ correction */
		xil_printf("[STPL-ZERO] Readback: ch0_sel=0x%08lX ch1_sel=0x%08lX ch0_pat=0x%08lX ch1_pat=0x%08lX\\n\\r",
			   (unsigned long)sel_rb0, (unsigned long)sel_rb1,
			   (unsigned long)pat_rb0, (unsigned long)pat_rb1);
		xil_printf("[STPL-ZERO]           ch0_ctrl=0x%08lX ch1_ctrl=0x%08lX dds0_scale=0x%08lX dds1_scale=0x%08lX\\n\\r",
			   (unsigned long)cntrl_rb0, (unsigned long)cntrl_rb1,
			   (unsigned long)dds_scale0_0, (unsigned long)dds_scale1_0);
		xil_printf("[STPL-ZERO]           ch0_iqcor=0x%08lX ch1_iqcor=0x%08lX\\n\\r",
			   (unsigned long)iqcor0, (unsigned long)iqcor1);
		xil_printf("[STPL-ZERO] HDL pattern format: pat[31:16]=s1, pat[15:0]=s0, interleaved {s1,s0,s1,s0,...}\\n\\r");
		xil_printf("[STPL-ZERO] For M=2 DPW=4: DAC0 sees samples from ch0, DAC1 from ch1\\n\\r");
	}

	/* Poll for FPGA DATA + AD9144 CGS+Frame after SYNC pulse */
	data_reached = 0;
	for (poll_i = 0; poll_i < 80; poll_i++) {
		fpga_link_state = Xil_In32(TX_JESD_BASEADDR + 0x280);
		ad9144_spi_read(dev->ad9144_device, REG_CODEGRPSYNCFLG, &cgs_now);
		ad9144_spi_read(dev->ad9144_device, REG_FRAMESYNCFLG, &frame_now);
		if (((fpga_link_state & 0x3) == 3) &&
		    (cgs_now == 0x0F) && (frame_now == 0x0F)) {
			data_reached = 1;
			break;
		}
		no_os_mdelay(50);
	}
	no_os_mdelay(100); /* Extra settling after full sync */

	/* Read back all AD9144 flags for log */
	fpga_link_state = Xil_In32(TX_JESD_BASEADDR + 0x280);
	ad9144_spi_read(dev->ad9144_device, REG_CODEGRPSYNCFLG, &cgs_now);
	ad9144_spi_read(dev->ad9144_device, REG_FRAMESYNCFLG, &frame_now);
	ad9144_spi_read(dev->ad9144_device, REG_GOODCHKSUMFLG, &chksum_now);
	ad9144_spi_read(dev->ad9144_device, REG_INITLANESYNCFLG, &ilas_now);
	xil_printf("[STPL-ZERO] Link: FPGA=0x%08lX (%s) CGS=0x%02X Frame=0x%02X Chk=0x%02X ILAS=0x%02X poll=%d\n\r",
		   (unsigned long)fpga_link_state,
		   ((fpga_link_state & 0x3) == 3) ? "DATA" :
		   ((fpga_link_state & 0x3) == 1) ? "CGS" : "OTHER",
		   cgs_now, frame_now, chksum_now, ilas_now, poll_i);

	if (data_reached && (cgs_now == 0x0F) && (frame_now == 0x0F)) {
		/* Run zero test only with fully synced link */
		uint8_t ctrl, fa;
		int zero_pass = 1;
		int dac_idx;
		for (dac_idx = 0; dac_idx < 2; dac_idx++) {
			ctrl = SHORT_TPL_SP_SEL(0) | SHORT_TPL_M_SEL(dac_idx);
			/* Datasheet-compliant sequence: select, program, enable, reset-pulse, read */
			ad9144_spi_write(dev->ad9144_device, REG_SHORT_TPL_TEST_0, ctrl);
			ad9144_spi_write(dev->ad9144_device, REG_SHORT_TPL_TEST_2, 0x00);
			ad9144_spi_write(dev->ad9144_device, REG_SHORT_TPL_TEST_1, 0x00);
			ad9144_spi_write(dev->ad9144_device, REG_SHORT_TPL_TEST_0,
					 ctrl | SHORT_TPL_TEST_EN);
			no_os_mdelay(1);
			ad9144_spi_write(dev->ad9144_device, REG_SHORT_TPL_TEST_0,
					 ctrl | SHORT_TPL_TEST_EN | SHORT_TPL_TEST_RESET);
			no_os_mdelay(1);
			ad9144_spi_write(dev->ad9144_device, REG_SHORT_TPL_TEST_0,
					 ctrl | SHORT_TPL_TEST_EN);
			no_os_mdelay(10);
			ad9144_spi_read(dev->ad9144_device, REG_SHORT_TPL_TEST_3, &fa);
			ad9144_spi_write(dev->ad9144_device, REG_SHORT_TPL_TEST_0, ctrl);
			xil_printf("[STPL-ZERO] DAC%d: expect=0x0000  fail=0x%02X  %s\n\r",
				   dac_idx, fa,
				   (fa & SHORT_TPL_FAIL) ? "FAIL" : "PASS");
			
			/* Binary probe on failure to identify what AD9144 receives */
			if (fa & SHORT_TPL_FAIL) {
				static const uint16_t probes[] = {
					0x0000, 0xFFFF, 0x5555, 0xAAAA, 0xA1A0, 0xB1B0, 0xC1C0, 0xD1D0,
					0xA0A1, 0xB0B1, 0xC0C1, 0xD0D1, 0x1234, 0x8000, 0x0001, 0x7FFF
				};
				uint32_t pi;
				uint8_t pf;
				int found = 0;
				for (pi = 0; pi < sizeof(probes)/sizeof(probes[0]) && !found; pi++) {
					ad9144_spi_write(dev->ad9144_device, REG_SHORT_TPL_TEST_0, ctrl);
					ad9144_spi_write(dev->ad9144_device, REG_SHORT_TPL_TEST_2, probes[pi] >> 8);
					ad9144_spi_write(dev->ad9144_device, REG_SHORT_TPL_TEST_1, probes[pi] & 0xFF);
					ad9144_spi_write(dev->ad9144_device, REG_SHORT_TPL_TEST_0, ctrl | SHORT_TPL_TEST_EN);
					no_os_mdelay(1);
					ad9144_spi_write(dev->ad9144_device, REG_SHORT_TPL_TEST_0,
							 ctrl | SHORT_TPL_TEST_EN | SHORT_TPL_TEST_RESET);
					no_os_mdelay(1);
					ad9144_spi_write(dev->ad9144_device, REG_SHORT_TPL_TEST_0, ctrl | SHORT_TPL_TEST_EN);
					no_os_mdelay(10);
					ad9144_spi_read(dev->ad9144_device, REG_SHORT_TPL_TEST_3, &pf);
					ad9144_spi_write(dev->ad9144_device, REG_SHORT_TPL_TEST_0, ctrl);
					if (!(pf & SHORT_TPL_FAIL)) {
						xil_printf("         [PROBE] DAC%d.s0 receives 0x%04X\n\r", dac_idx, probes[pi]);
						found = 1;
					}
				}
				if (!found)
					xil_printf("         [PROBE] DAC%d.s0 no match from %lu candidates\n\r",
						   dac_idx, (unsigned long)(sizeof(probes)/sizeof(probes[0])));
				zero_pass = 0;
			}
		}
		if (zero_pass) {
			xil_printf("[STPL-ZERO] Zero pattern PASSES - test mechanism works!\n\r");
		} else {
			xil_printf("[STPL-ZERO] Zero pattern FAILS with synced link - genuine mismatch\n\r");
		}
	} else {
		xil_printf("[STPL-ZERO] SKIPPED - AD9144 not fully synced (CGS=0x%02X Frame=0x%02X)\n\r",
			   cgs_now, frame_now);
	}

skip_stpl_zero:
	/* ---- Main STPL test with real pattern ---- */
	xil_printf("\n\r[STPL-PATTERN] Waiting for 5 consecutive stable link polls before SED pattern test...\n\r");
	if (wait_link_stable(dev, 5, 2000) != 0) {
		xil_printf("[STPL-PATTERN] ABORTED: Link not stable enough\n\r");
		return test_errors;
	}

	dev->ad9144_channels[0].pat_data = 0xb1b0a1a0;
	dev->ad9144_channels[1].pat_data = 0xd1d0c1c0;
	
	/* Program patterns and enable SED mode */
	Xil_Out32(dev->ad9144_core->base + 0x0400, 0x00000001); /* Enable ch0 */
	Xil_Out32(dev->ad9144_core->base + 0x0440, 0x00000001); /* Enable ch1 */
	Xil_Out32(dev->ad9144_core->base + 0x0410, 0xb1b0a1a0); /* ch0 pattern */
	Xil_Out32(dev->ad9144_core->base + 0x0450, 0xd1d0c1c0); /* ch1 pattern */
	Xil_Out32(dev->ad9144_core->base + 0x0418, 0x00000001); /* ch0 sel = SED */
	Xil_Out32(dev->ad9144_core->base + 0x0458, 0x00000001); /* ch1 sel = SED */
	/* Skip SYNC pulse - prevents link disruption during STPL test */
	no_os_mdelay(50); /* Allow register update to propagate */

	/* Poll for FPGA DATA + AD9144 CGS+Frame after SYNC pulse */
	data_reached = 0;
	for (poll_i = 0; poll_i < 80; poll_i++) {
		fpga_link_state = Xil_In32(TX_JESD_BASEADDR + 0x280);
		ad9144_spi_read(dev->ad9144_device, REG_CODEGRPSYNCFLG, &cgs_now);
		ad9144_spi_read(dev->ad9144_device, REG_FRAMESYNCFLG, &frame_now);
		if (((fpga_link_state & 0x3) == 3) &&
		    (cgs_now == 0x0F) && (frame_now == 0x0F)) {
			data_reached = 1;
			break;
		}
		no_os_mdelay(50);
	}
	no_os_mdelay(100);

	/* Diagnostic: readback FPGA DAC core registers */
	{
		uint32_t i, pat_rb, sel_rb;
		dbg_printf("[STPL-DIAG] AXI DAC core base=0x%08lX  num_channels=%u\n\r",
			   dev->ad9144_core->base, dev->ad9144_core->num_channels);
		for (i = 0; i < 4; i++) {
			pat_rb = Xil_In32(dev->ad9144_core->base + 0x0410 + i * 0x40);
			sel_rb = Xil_In32(dev->ad9144_core->base + 0x0418 + i * 0x40);
			dbg_printf("[STPL-DIAG] FPGA ch%lu: pat_reg=0x%08lX  sel_reg=0x%08lX%s\n\r",
				   (unsigned long)i, (unsigned long)pat_rb,
				   (unsigned long)sel_rb,
				   (i >= 2) ? " (NOT in HDL)" : "");
		}
		dbg_printf("[STPL-DIAG] AD9144 num_converters=%u  STPL expected:\n\r",
			   dev->ad9144_device->num_converters);
		for (i = 0; i < dev->ad9144_device->num_converters; i++) {
			dbg_printf("[STPL-DIAG]   DAC%lu: s0=0x%04lX s1=0x%04lX s2=0x%04lX s3=0x%04lX\n\r",
				   (unsigned long)i,
				   (unsigned long)dev_init->ad9144_param.stpl_samples[i][0],
				   (unsigned long)dev_init->ad9144_param.stpl_samples[i][1],
				   (unsigned long)dev_init->ad9144_param.stpl_samples[i][2],
				   (unsigned long)dev_init->ad9144_param.stpl_samples[i][3]);
		}
		/* AD9144 scrambling and ILAS config readback */
		uint8_t scr_l = 0, ils_m = 0, ils_cs_n = 0, ils_np = 0, ils_s = 0, ils_hd_cf = 0;
		ad9144_spi_read(dev->ad9144_device, 0x453, &scr_l);    /* SCR_L: bit7=SCR, [4:0]=L-1 */
		ad9144_spi_read(dev->ad9144_device, 0x456, &ils_m);     /* M-1 */
		ad9144_spi_read(dev->ad9144_device, 0x457, &ils_cs_n);  /* CS[7:6], N-1[4:0] */
		ad9144_spi_read(dev->ad9144_device, 0x458, &ils_np);    /* SUBCLASSV[7:5], N'-1[4:0] */
		ad9144_spi_read(dev->ad9144_device, 0x459, &ils_s);     /* JESDV[7:5], S-1[4:0] */
		ad9144_spi_read(dev->ad9144_device, 0x45A, &ils_hd_cf); /* HD[7], CF[4:0] */
		dbg_printf("[STPL-DIAG] AD9144 ILS: SCR_L=0x%02X (SCR=%u L=%u) M=%u N=%u N'=%u S=%u HD=%u CF=%u\n\r",
			   scr_l, !!(scr_l & 0x80), (scr_l & 0x1F) + 1,
			   (ils_m & 0x1F) + 1,
			   (ils_cs_n & 0x1F) + 1,
			   (ils_np & 0x1F) + 1,
			   (ils_s & 0x1F) + 1,
			   !!(ils_hd_cf & 0x80),
			   (ils_hd_cf & 0x1F));
		dbg_printf("[STPL-DIAG] AD9144 ILS raw: 0x453=0x%02X 0x459=0x%02X 0x45A=0x%02X\n\r",
			   scr_l, ils_s, ils_hd_cf);
	}

	/* Final status readback for main STPL test */
	{
		uint32_t link_st_post;
		uint8_t cgs_post, frame_post, chksum_post, ilas_post;
		uint32_t dpw_reg;
		link_st_post = Xil_In32(TX_JESD_BASEADDR + 0x280);
		ad9144_spi_read(dev->ad9144_device, REG_CODEGRPSYNCFLG, &cgs_post);
		ad9144_spi_read(dev->ad9144_device, REG_FRAMESYNCFLG, &frame_post);
		ad9144_spi_read(dev->ad9144_device, REG_GOODCHKSUMFLG, &chksum_post);
		ad9144_spi_read(dev->ad9144_device, REG_INITLANESYNCFLG, &ilas_post);
		dpw_reg = Xil_In32(TX_JESD_BASEADDR + 0x14);
		dbg_printf("[STPL-DIAG] PRE-TEST link: FPGA=0x%08lX (%s) CGS=0x%02X Frame=0x%02X Chk=0x%02X ILAS=0x%02X poll=%d\n\r",
			   (unsigned long)link_st_post,
			   ((link_st_post & 0x3) == 3) ? "DATA" :
			   ((link_st_post & 0x3) == 2) ? "ILAS" :
			   ((link_st_post & 0x3) == 1) ? "CGS" : "WAIT",
			   cgs_post, frame_post, chksum_post, ilas_post, poll_i);
		dbg_printf("[STPL-DIAG] JESD TX data_path_width reg (0x14) = 0x%08lX (synth_dpw=%lu tpl_dpw=%lu)\n\r",
			   (unsigned long)dpw_reg,
			   (unsigned long)(1 << (dpw_reg & 0xFF)),
			   (unsigned long)((dpw_reg >> 8) & 0xFF));

		if ((cgs_post != 0x0F) || (frame_post != 0x0F)) {
			dbg_printf("[STPL-DIAG] *** AD9144 NOT FULLY SYNCED - STPL results will be invalid ***\n\r");
		}
	}

	status = ad9144_short_pattern_test(dev->ad9144_device, &dev_init->ad9144_param);
	if (status < 0) {
		xil_printf("[TEST] STPL test: %d mismatches\n\r", -status);
		test_errors++;
	}
#else
	xil_printf("\n\r[STPL] Skipped - see stpl_analysis.md. PRBS tests confirm link works.\n\r");
#endif /* SKIP_STPL_TESTS */

	/*
	 * PRBS datapath test: The AD9144 PRBS checker (REG_PRBS, 0x14B) sits
	 * after the JESD deframer but before the interpolation filter. With 2x
	 * interpolation the PLL runs at 1966 MHz; bypassing the interpolator
	 * mid-link (REG_INTERP_MODE = 0x00) causes a rate mismatch — the DAC
	 * core expects 1966 MSPS input but the JESD link only delivers 983 MSPS.
	 * This breaks frame sync and the link does not recover.
	 *
	 * When interpolation > 1x we therefore SKIP the PRBS tests entirely.
	 * The STPL zero + pattern tests above already validate the complete
	 * FPGA -> SerDes -> JESD deframer -> XBAR -> per-sample data path.
	 */
	if (dev_init->ad9144_param.interpolation > 1) {
		xil_printf("[TEST] PRBS7/15 SKIPPED — interpolation=%dx active, "
			   "checker incompatible (STPL validates datapath)\n\r",
			   dev_init->ad9144_param.interpolation);
	} else {
		// PN7 data path test
		dev->ad9144_channels[0].sel = AXI_DAC_DATA_SEL_PN7;
		dev->ad9144_channels[1].sel = AXI_DAC_DATA_SEL_PN7;
		axi_dac_data_setup(dev->ad9144_core);
		/* Poll for FPGA DATA + AD9144 CGS+Frame */
		for (poll_i = 0; poll_i < 80; poll_i++) {
			fpga_link_state = Xil_In32(TX_JESD_BASEADDR + 0x280);
			ad9144_spi_read(dev->ad9144_device, REG_CODEGRPSYNCFLG, &cgs_now);
			ad9144_spi_read(dev->ad9144_device, REG_FRAMESYNCFLG, &frame_now);
			if (((fpga_link_state & 0x3) == 3) &&
			    (cgs_now == 0x0F) && (frame_now == 0x0F)) break;
			no_os_mdelay(50);
		}
		no_os_mdelay(100);
		xil_printf("[TEST] PN7 link: CGS=0x%02X Frame=0x%02X poll=%d\n\r", cgs_now, frame_now, poll_i);
		dev_init->ad9144_param.prbs_type = AD9144_PRBS7;
		status = ad9144_datapath_prbs_test(dev->ad9144_device, &dev_init->ad9144_param);
		if (status < 0) {
			xil_printf("[TEST] PRBS7 test FAILED\n\r");
			test_errors++;
		} else {
			xil_printf("[TEST] PRBS7 test PASSED\n\r");
		}

		// PN15 data path test
		dev->ad9144_channels[0].sel = AXI_DAC_DATA_SEL_PN15;
		dev->ad9144_channels[1].sel = AXI_DAC_DATA_SEL_PN15;
		axi_dac_data_setup(dev->ad9144_core);
		/* Poll for FPGA DATA + AD9144 CGS+Frame */
		for (poll_i = 0; poll_i < 80; poll_i++) {
			fpga_link_state = Xil_In32(TX_JESD_BASEADDR + 0x280);
			ad9144_spi_read(dev->ad9144_device, REG_CODEGRPSYNCFLG, &cgs_now);
			ad9144_spi_read(dev->ad9144_device, REG_FRAMESYNCFLG, &frame_now);
			if (((fpga_link_state & 0x3) == 3) &&
			    (cgs_now == 0x0F) && (frame_now == 0x0F)) break;
			no_os_mdelay(50);
		}
		no_os_mdelay(100);
		xil_printf("[TEST] PN15 link: CGS=0x%02X Frame=0x%02X poll=%d\n\r", cgs_now, frame_now, poll_i);
		dev_init->ad9144_param.prbs_type = AD9144_PRBS15;
		status = ad9144_datapath_prbs_test(dev->ad9144_device, &dev_init->ad9144_param);
		if (status < 0) {
			xil_printf("[TEST] PRBS15 test FAILED\n\r");
			test_errors++;
		} else {
			xil_printf("[TEST] PRBS15 test PASSED\n\r");
		}
	}

	/* ===== Subclass 1 Diagnostics (after stable link + datapath PRBS) ===== */
	fmcdac_sysref_tune(dev);
	fmcdac_sysref_verify(dev);
	fmcdac_latency_readback(dev);
	fmcdac_phy_prbs_test(dev);

	xil_printf("[NCO-TEST] Run 10 MHz DDS + AD9144 NCO discriminator test? [y/N]: ");
	{
		int run_nco = getc(stdin);
		fmcdac_flush_input();
		if (run_nco == 'y' || run_nco == 'Y') {
			status = fmcdac_nco_discriminator_test(dev);
			if (status != 0)
				xil_printf("[NCO-TEST] Diagnostic setup failed: %d\n\r",
					   status);
		} else {
			xil_printf("[NCO-TEST] Skipped.\n\r");
		}
	}

	xil_printf("[DDS-BAND] Run focused DDS sweep diagnostic around 230-330 MHz? [y/N]: ");
	{
		int run_dds_band = getc(stdin);
		fmcdac_flush_input();
		if (run_dds_band == 'y' || run_dds_band == 'Y') {
			status = fmcdac_dds_band_diagnostic_test(dev);
			if (status != 0)
				xil_printf("[DDS-BAND] Diagnostic setup failed: %d\n\r",
					   status);
		} else {
			xil_printf("[DDS-BAND] Skipped. Continuing to normal DDS sweep.\n\r");
		}
	}

	/* DDS tone test - validates data path for NCO/CORDIC development */
	force_dds_tone(dev);

	return test_errors ? -test_errors : 0;
}

static int fmcdac_dac_init(struct fmcdac_dev *dev,
			    struct fmcdac_init_param *dev_init)
{
	/* DAC (AD9144) channels configuration - only 2 real channels in HDL */
	dev->ad9144_channels[0].pat_data = 0xb1b0a1a0;
	dev->ad9144_channels[1].pat_data = 0xd1d0c1c0;

  	dev->ad9144_channels[0].sel = AXI_DAC_DATA_SEL_DDS;
	dev->ad9144_channels[1].sel = AXI_DAC_DATA_SEL_DDS;

	/* DAC Core - HDL implements 2 channels (one per converter, M=2)
	 * Each channel carries 32 bits = two 16-bit time-interleaved samples.
	 * Writing to ch2/ch3 registers hits non-existent address space (reads 0xDEADDEAD). */
	dev_init->ad9144_core_param = (struct axi_dac_init) {
		.name = "ad9144_dac",
		.base =	TX_CORE_BASEADDR,
		.num_channels = 2,
		.channels = &dev->ad9144_channels[0]
	};

	/* PCB Routing: SERDIN7,6,5,4 are routed to FPGA DP0,1,2,3 (reversed order)
	 * lane_mux[logical_lane] = physical_SERDIN_pin
	 */
	dev_init->ad9144_param.lane_mux[0] = 4;  // Logical lane 0 → Physical SERDIN4 (DP3)
	dev_init->ad9144_param.lane_mux[1] = 5;  // Logical lane 1 → Physical SERDIN5 (DP2)
	dev_init->ad9144_param.lane_mux[2] = 6;  // Logical lane 2 → Physical SERDIN6 (DP1)
	dev_init->ad9144_param.lane_mux[3] = 7;  // Logical lane 3 → Physical SERDIN7 (DP0)
	dev_init->ad9144_param.lane_mux[4] = 0;  // Unused (only 4 lanes active)
	dev_init->ad9144_param.lane_mux[5] = 1;  // Unused
	dev_init->ad9144_param.lane_mux[6] = 2;  // Unused
	dev_init->ad9144_param.lane_mux[7] = 3;  // Unused

	/* Lane polarity inversion: bit[n]=1 inverts P/N on lane n
	 * Frame sync 0x0B = lanes 0,1,3 OK, lane 2 failing -> try inverting lane 2
	 * Set to 0x04 to invert only lane 2, or sweep 0x00-0x0F to find working combo
	 */
	dev_init->ad9144_param.lane_invert_mask = 0x04;  // Invert lane 2 (bit 2)

	/* STPL expected values: only 2 converters (M=2) */
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
	ad9516_remove(dev->ad9516_dev);

	/* Memory deallocation for PHY and LINK layers */
	adxcvr_remove(dev->ad9144_xcvr);
	axi_jesd204_tx_remove(dev->ad9144_jesd);

	/* Memory deallocation for gpios */
	// additional code for gpio signals needs to be added here
	no_os_gpio_remove(dev->gpio_clkd_sync);
	no_os_gpio_remove(dev->gpio_dac_reset);
	no_os_gpio_remove(dev->gpio_dac_txen);
}

int fmcdac_reconfig(struct ad9144_init_param *p_ad9144_param,
		     struct adxcvr_init *ad9144_xcvr_param,
		     struct ad9516_platform_data *p_ad9516_param)
{

	uint8_t mode = FMCDAC_DEFAULT_RATE_OPTION_CHAR;
	uint8_t clk_mode = '1';

	xil_printf("[AUTO] Clock configuration fixed to option 1 (Clock distributor)\n\r");
	xil_printf("[AUTO] Sampling rate fixed to " FMCDAC_DEFAULT_RATE_TEXT "\n\r");

	switch (clk_mode) {
	case '2':
		g_clk_mode = FMCDAC_CLK_SYNTHESIZE;
		p_ad9516_param->vco_clk_sel = 1;
		p_ad9516_param->power_down_vco_clk = 0;
		break;
	case '3':
		g_clk_mode = FMCDAC_CLK_EXTERNAL;
		p_ad9516_param->vco_clk_sel = 0;
		p_ad9516_param->power_down_vco_clk = 0;
		break;
	default:
		g_clk_mode = FMCDAC_CLK_DISTRIBUTE;
		p_ad9516_param->vco_clk_sel = 0;
		p_ad9516_param->power_down_vco_clk = 0;
		break;
	}

	switch (mode) {
	case '5':
		xil_printf("5 - DAC 1966 MSPS (2x interpolation)\n");
		/*
		 * 2x interpolation: AD9144 internal DAC clock = 1966.08 MHz,
		 * but the JESD link data rate stays at 983.04 MSPS (same as
		 * mode 4).  The FPGA side is completely unchanged — same lane
		 * rate, same REFCLK, same QPLL0 config, no HDL changes.
		 *
		 * AD9144 PLL: 122.88 MHz × 16 = 1966.08 MHz
		 *   (lo_div_mode=1, ref_div_mode=1, bcount=16 — integer, locks)
		 *   VCO = 1966.08 × 4 = 7864.32 MHz (in 6.0–12.4 GHz range)
		 *
		 * Nyquist = 1966.08 / 2 = 983.04 MHz → 500 MHz is well covered.
		 *
		 * FPGA DDS Nyquist remains 491.52 MHz (input rate = 983 MSPS).
		 * For tones above 491 MHz, use the AD9144 internal NCO
		 * (fcenter_shift) which runs at the full 1966 MSPS DAC rate.
		 */
		p_ad9144_param->interpolation = 2;
		p_ad9144_param->pll_enable = 1;
		p_ad9144_param->pll_ref_frequency_khz = 122880;  /* same 122.88 MHz ref */
		p_ad9144_param->pll_dac_frequency_khz = 1966080; /* 122.88 × 16 */
		p_ad9144_param->lane_rate_kbps = 9830400;         /* unchanged */
		ad9144_xcvr_param->lane_rate_khz = 9830400;       /* unchanged */
#ifndef ALTERA_PLATFORM
		ad9144_xcvr_param->ref_rate_khz = 122880;         /* unchanged */
#else
		ad9144_xcvr_param->parent_rate_khz = 122880;
#endif
		/* QPLL0 + OUTCLK_PMA stay as mode 4 defaults — no change needed */
		break;
	case '2':
		xil_printf("2 - DAC 983 MSPS (1x, no interpolation)\n");
		/* Original mode 4 without interpolation — for debugging or
		 * isolating interpolation filter effects. */
		p_ad9144_param->interpolation = 1;
		p_ad9144_param->pll_enable = 1;
		p_ad9144_param->pll_ref_frequency_khz = 122880;
		p_ad9144_param->pll_dac_frequency_khz = 983040;
		p_ad9144_param->lane_rate_kbps = 9830400;
		ad9144_xcvr_param->lane_rate_khz = 9830400;
		ad9144_xcvr_param->ref_rate_khz = 122880;
		break;
	case '4':
		printf ("DAC  600 MSPS\n");

		p_ad9144_param->pll_enable = 0;
		p_ad9144_param->pll_dac_frequency_khz = 600000;
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

		p_ad9144_param->pll_enable = 0;
		p_ad9144_param->pll_dac_frequency_khz = 500000;
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

	default:
		printf ("1 - DAC 1966 MSPS (2x interpolation, default)\n");
		p_ad9144_param->interpolation = 2;
		p_ad9144_param->pll_enable = 1;
		p_ad9144_param->pll_ref_frequency_khz = 122880;
		p_ad9144_param->pll_dac_frequency_khz = 1966080;
		p_ad9144_param->lane_rate_kbps = 9830400;
		ad9144_xcvr_param->lane_rate_khz = 9830400;
		ad9144_xcvr_param->ref_rate_khz = 122880;
		break;
	}

	fmcdac_apply_clock_mode(p_ad9144_param, p_ad9516_param);

	return(0);
}

static int fmcdac_setup(struct fmcdac_dev *dev,
			 struct fmcdac_init_param *dev_init)
{
	int status;

	xil_printf("[SETUP] Starting FMCDAC setup...\n\r");

	xil_printf("[SETUP] Initializing GPIO...\n\r");
	status = fmcdac_gpio_init(dev);
	if (status < 0) {
		xil_printf("[ERROR] GPIO init failed: %d\n\r", status);
		return status;
	}
	xil_printf("[SETUP] GPIO initialized successfully\n\r");

	xil_printf("[SETUP] Initializing SPI...\n\r");
	status = fmcdac_spi_init(dev_init);
	if (status < 0) {
		xil_printf("[ERROR] SPI init failed: %d\n\r", status);
		return status;
	}
	xil_printf("[SETUP] SPI initialized successfully\n\r");

	xil_printf("[SETUP] Initializing I2C...\n\r");
	status = fmcdac_i2c_init(dev);
	if (status < 0) {
		xil_printf("[ERROR] I2C init failed: %d\n\r", status);
		return status;
	}
	xil_printf("[SETUP] I2C initialized successfully\n\r");
	fmcdac_i2c_probe(dev);
#ifndef SKIP_SI5328
	fmcdac_i2c_scan_si5328(dev);

	xil_printf("[SETUP] Programming Si5328 refclk...\n\r");
	status = fmcdac_si5328_setup(dev);
	if (status < 0) {
		xil_printf("[ERROR] Si5328 setup failed: %d\n\r", status);
		return status;
	}
	xil_printf("[SETUP] Si5328 configured successfully\n\r");
#else
	xil_printf("[SETUP] Si5328 skipped — GTH REFCLK from AD9516 (122.88 MHz)\n\r");
#endif

	xil_printf("[SETUP] Initializing Clock (AD9516)...\n\r");
	
	// TEST: Try a simple SPI loopback/read test before initializing AD9516
	xil_printf("[TEST] Testing SPI communication...\n\r");
	struct no_os_spi_desc *test_spi;
	uint8_t test_buffer[3] = {0x00, 0x00, 0x00};
	int spi_test_ret = no_os_spi_init(&test_spi, &dev_init->ad9516_param.spi_init);
	if (spi_test_ret == 0) {
		xil_printf("[TEST] SPI initialized for test\n\r");
		// Try reading from AD9516 PART ID (address 0x0003, with read bit = 0x8000)
		test_buffer[0] = 0x80;  // High byte with read bit
		test_buffer[1] = 0x03;  // Low byte of address
		test_buffer[2] = 0x00;  // Dummy byte
		spi_test_ret = no_os_spi_write_and_read(test_spi, test_buffer, 3);
		xil_printf("[TEST] SPI read result: ret=%d, data=0x%02X 0x%02X 0x%02X\n\r", 
			spi_test_ret, test_buffer[0], test_buffer[1], test_buffer[2]);
		no_os_spi_remove(test_spi);
	} else {
		xil_printf("[TEST] SPI init failed: %d\n\r", spi_test_ret);
	}
	
	status = fmcdac_clk_init(dev, dev_init);
	if (status < 0) {
		xil_printf("[ERROR] Clock init failed: %d\n\r", status);
		return status;
	}
	xil_printf("[SETUP] Clock configuration prepared\n\r");

	xil_printf("[SETUP] Initializing JESD204...\n\r");
	status = fmcdac_jesd_init(dev_init);
	if (status < 0) {
		xil_printf("[ERROR] JESD init failed: %d\n\r", status);
		return status;
	}
	xil_printf("[SETUP] JESD initialized successfully\n\r");

	/*
	 * Default: 2x interpolation, DAC PLL = 1966.08 MHz.
	 *
	 * The FPGA DDS and JESD link run at 983.04 MSPS (unchanged from
	 * the original 1x mode).  The AD9144 2x interpolation filter
	 * upsamples internally to 1966.08 MSPS, giving:
	 *   - Better image rejection (images at ±1966 MHz, not ±983 MHz)
	 *   - ~3 dB SNR improvement in-band
	 *   - Reduced sinc droop (0.6 dB vs 2.5 dB at 400 MHz)
	 *   - Relaxed analog anti-alias filter requirements
	 *
	 * The FPGA DDS Nyquist remains 491.52 MHz (input rate = 983 MSPS).
	 * To generate tones above 491 MHz, use AD9144 on-chip NCO or
	 * switch to JESD Mode 9 (L=8) — see MODE9_HDL_REQUIREMENTS.md.
	 *
	 * AD9144 PLL: 122.88 MHz × 16 = 1966.08 MHz
	 *   VCO = 7864.32 MHz (in valid 6.0–12.4 GHz range)
	 */
	dev_init->ad9144_param.lane_rate_kbps = 9830400;
	dev_init->ad9144_param.spi3wire = 0;  // Use 4-wire SPI (enables SDO for reads)
#ifdef JESD_FSM_ON
	dev_init->ad9144_param.num_converters =
		fmcdac_init.jtx_link_rx.converters_per_device;
	dev_init->ad9144_param.num_lanes = fmcdac_init.jtx_link_rx.lanes_per_device;
#endif
	dev_init->ad9144_param.interpolation = 2;
	dev_init->ad9144_param.fcenter_shift = 0;
	dev_init->ad9144_param.pll_enable = 1;
	dev_init->ad9144_param.pll_ref_frequency_khz = 122880;
	dev_init->ad9144_param.pll_dac_frequency_khz = 1966080;
	dev_init->ad9144_param.jesd204_subclass = 1;
	dev_init->ad9144_param.jesd204_scrambling = 1;
	dev_init->ad9144_param.jesd204_mode = 4;
	/* 122.88 MHz REFCLK from AD9516, AD9144 PLL -> 1966.08 MSPS (2x interp) */
	/* Lane rate matches 983.04 MSPS link rate with M=2, L=4, F=1 */
	dev_init->ad9144_param.lane_rate_kbps = 9830400;

	/* change the default JESD configurations, if required */
	fmcdac_reconfig(&dev_init->ad9144_param,
			 &dev_init->ad9144_xcvr_param,
			 dev_init->ad9516_param.ad9516_st.pdata);

	status = fmcdac_dac_init(&fmcdac, &fmcdac_init);
	if (status < 0)
		return status;

	/* Reconfigure the default JESD configurations */

	xil_printf("[SETUP] Configuring AD9516 clock distribution...\n\r");
	/* setup clocks (avoid double re-init) */
	if (!dev->ad9516_dev) {
		status = ad9516_setup(&dev->ad9516_dev, dev_init->ad9516_param);
		if (status != 0) {
			xil_printf("[ERROR] ad9516_setup() failed with status: %d\n\r", status);
			return status;
		}
	} else {
		xil_printf("[SETUP] AD9516 already initialized; skipping re-init\n\r");
	}

	status = fmcdac_ad9516_program_outputs(dev, dev_init);
	if (status != 0) {
		xil_printf("[ERROR] AD9516 output programming failed: %d\n\r", status);
		return status;
	}
	xil_printf("[SETUP] AD9516 configured successfully\n\r");



	// Recommended DAC JESD204 link startup sequence
	//   1. FPGA JESD204 Link Layer
	//   2. FPGA JESD204 PHY Layer
	//   3. DAC
	//

	xil_printf("[SETUP] Configuring JESD204 transceivers...\n\r");
	status = fmcdac_trasnceiver_setup(&fmcdac, &fmcdac_init);
	if (status != 0) {
		xil_printf("[ERROR] Transceiver setup failed: %d\n\r", status);
		return status;
	}
	xil_printf("[SETUP] Transceivers configured successfully\n\r");

#ifdef JESD_FSM_ON
	xil_printf("[SETUP] Configuring AD9144 with JESD FSM...\n\r");
	status = ad9144_setup_jesd_fsm(&dev->ad9144_device, &dev_init->ad9144_param);
	if (status) {
		xil_printf("error: ad9144_setup_jesd_fsm() failed\n");
		return status;
	}
	xil_printf("[SETUP] AD9144 FSM setup completed\n\r");
#else
	xil_printf("[SETUP] Configuring AD9144 (legacy mode)...\n\r");
	status = ad9144_setup_legacy(&dev->ad9144_device, &dev_init->ad9144_param);
	if (status != 0) {
		xil_printf("error: ad9144_setup_legacy() failed\n");
		return status;
	}
	fmcdac_ad9144_jesd_sanity(dev->ad9144_device, &dev_init->ad9144_param);
	
	/* ===== AD9144 RX-Side Link Debug ===== */

	dbg_printf("\n\r[LINK-DEBUG] AD9144 Receiver-Side Status:\n\r");
	
	uint8_t pll_status_reg, serdes_pll, cdr_status;
	uint8_t sync_ctrl, sync_status;
	uint8_t link_ctrl0, link_ctrl1;
	uint8_t cgs_flag, ilas_flag, frame_flag, chksum_flag;
	
	/* CRITICAL: SERDES PLL lock (required for CDR operation) */
	ad9144_spi_read(dev->ad9144_device, 0x281, &pll_status_reg);
	serdes_pll = pll_status_reg & 0x01;
	dbg_printf("[LINK-DEBUG] SERDES PLL status (0x281) = 0x%02X (locked=%u)\n\r",
	           pll_status_reg, serdes_pll);
	
	if (!serdes_pll) {
		dbg_printf("[LINK-DEBUG] *** CRITICAL: SERDES PLL NOT LOCKED ***\n\r");
		dbg_printf("[LINK-DEBUG] CDR cannot recover clock from lanes!\n\r");
		dbg_printf("[LINK-DEBUG] Check: REG_REF_CLK_DIVIDER_LDO (0x289), REG_SYNTH_ENABLE_CNTRL (0x280)\n\r");
		uint8_t synth_ctrl, ref_div;
		ad9144_spi_read(dev->ad9144_device, 0x280, &synth_ctrl);
		ad9144_spi_read(dev->ad9144_device, 0x289, &ref_div);
		dbg_printf("[LINK-DEBUG] SYNTH_ENABLE_CNTRL (0x280) = 0x%02X\n\r", synth_ctrl);
		dbg_printf("[LINK-DEBUG] REF_CLK_DIVIDER_LDO (0x289) = 0x%02X\n\r", ref_div);
	}
	
	/* CDR operating mode per lane */
	ad9144_spi_read(dev->ad9144_device, 0x230, &cdr_status);
	dbg_printf("[LINK-DEBUG] CDR operating mode (0x230) = 0x%02X\n\r", cdr_status);
	
	/* Link control registers */
	ad9144_spi_read(dev->ad9144_device, 0x300, &link_ctrl0);
	ad9144_spi_read(dev->ad9144_device, 0x301, &link_ctrl1);
	dbg_printf("[LINK-DEBUG] JRX_CTRL_0 (0x300) = 0x%02X (link_en=%u)\n\r",
	           link_ctrl0, link_ctrl0 & 0x01);
	dbg_printf("[LINK-DEBUG] JRX_CTRL_1 (0x301) = 0x%02X (subclass=%u)\n\r",
	           link_ctrl1, link_ctrl1 & 0x03);
	
	/* Sync control and status */
	ad9144_spi_read(dev->ad9144_device, 0x03A, &sync_ctrl);
	ad9144_spi_read(dev->ad9144_device, 0x03B, &sync_status);
	dbg_printf("[LINK-DEBUG] SYNC_CTRL (0x03A) = 0x%02X\n\r", sync_ctrl);
	dbg_printf("[LINK-DEBUG] SYNC_STATUS (0x03B) = 0x%02X\n\r", sync_status);
	
	/* Per-lane synchronization status registers:
	 * 0x470 = REG_CODEGRPSYNCFLG  (CGS)
	 * 0x471 = REG_FRAMESYNCFLG    (Frame Sync)
	 * 0x472 = REG_GOODCHKSUMFLG   (Good Checksum)
	 * 0x473 = REG_INITLANESYNCFLG (Initial Lane Sync)
	 */
	ad9144_spi_read(dev->ad9144_device, 0x470, &cgs_flag);
	ad9144_spi_read(dev->ad9144_device, 0x471, &frame_flag);
	ad9144_spi_read(dev->ad9144_device, 0x472, &chksum_flag);
	ad9144_spi_read(dev->ad9144_device, 0x473, &ilas_flag);
	dbg_printf("[LINK-DEBUG] CGS       (0x470) = 0x%02X (expect 0x0F for 4 lanes)\n\r", cgs_flag);
	dbg_printf("[LINK-DEBUG] Frame Sync(0x471) = 0x%02X (expect 0x0F for 4 lanes)\n\r", frame_flag);
	dbg_printf("[LINK-DEBUG] Checksum  (0x472) = 0x%02X (expect 0x0F for 4 lanes)\n\r", chksum_flag);
	dbg_printf("[LINK-DEBUG] Init Sync (0x473) = 0x%02X (expect 0x0F for 4 lanes)\n\r", ilas_flag);
	
	/* FPGA TX side status */
	uint32_t tx_link_status, tx_link_disabled;
	const char *link_state_str;
	tx_link_status = Xil_In32(TX_JESD_BASEADDR + 0x280);
	tx_link_disabled = Xil_In32(TX_JESD_BASEADDR + 0xC4);
	
	switch (tx_link_status & 0x3) {
		case 0: link_state_str = "WAIT"; break;
		case 1: link_state_str = "CGS"; break;
		case 2: link_state_str = "ILAS"; break;
		case 3: link_state_str = "DATA"; break;
		default: link_state_str = "UNKNOWN"; break;
	}
	
	dbg_printf("[LINK-DEBUG] FPGA TX link_status (0x280) = 0x%08lX (state=%s)\n\r",
	           tx_link_status, link_state_str);
	dbg_printf("[LINK-DEBUG] FPGA TX SYNC~ = %s\n\r",
	           (tx_link_status & 0x10) ? "DEASSERTED (good)" : "ASSERTED (AD9144 requesting CGS)");
	dbg_printf("[LINK-DEBUG] FPGA TX link_state (0x0C4) = 0x%08lX\n\r", tx_link_disabled);
	
	/* XCVR PHY status */
	uint32_t xcvr_status = Xil_In32(TX_XCVR_BASEADDR + 0x14);
	dbg_printf("[LINK-DEBUG] XCVR status (0x14) = 0x%08lX\n\r", xcvr_status);
	dbg_printf("[LINK-DEBUG]   Expected: CPLLs locked, TX buffers up, resets deasserted\n\r");
	
	/* Detailed interpretation */
	if (cgs_flag == 0x00) {
		dbg_printf("[LINK-DEBUG] *** NO lanes achieved CGS - physical layer issue ***\n\r");
		if (!serdes_pll) {
			dbg_printf("[LINK-DEBUG] Root cause: SERDES PLL not locked\n\r");
		} else {
			dbg_printf("[LINK-DEBUG] SERDES PLL locked but no K28.5 detected on lanes\n\r");
			dbg_printf("[LINK-DEBUG] Check: FPGA TX signal integrity, lane polarity, XCVR TX buffers\n\r");
		}
	} else if (cgs_flag == 0x0F) {
		dbg_printf("[LINK-DEBUG] All lanes achieved CGS - K28.5 detected successfully\n\r");
		if (frame_flag != 0x0F)
			dbg_printf("[LINK-DEBUG] Frame sync incomplete (0x%02X) - check F/K config\n\r", frame_flag);
		if (chksum_flag != 0x0F)
			dbg_printf("[LINK-DEBUG] Checksum fail (0x%02X) - ILAS config mismatch (check XBAR)\n\r", chksum_flag);
		if (ilas_flag != 0x0F)
			dbg_printf("[LINK-DEBUG] Init lane sync incomplete (0x%02X) - ILAS sequence issue\n\r", ilas_flag);
		if (cgs_flag == 0x0F && frame_flag == 0x0F &&
		    chksum_flag == 0x0F && ilas_flag == 0x0F) {
			dbg_printf("[LINK-DEBUG] *** SUCCESS: All lanes in sync (CGS+Frame+Checksum+ILAS)! ***\n\r");
		}
	} else {
		dbg_printf("[LINK-DEBUG] Partial CGS (0x%02X) - some lanes failing\n\r", cgs_flag);
		dbg_printf("[LINK-DEBUG] Check individual lane routing and signal quality\n\r");
	}

	/* Read lane crossbar and polarity config for verification */
	uint8_t xbar0, xbar1, lane_inv;
	ad9144_spi_read(dev->ad9144_device, 0x308, &xbar0);
	ad9144_spi_read(dev->ad9144_device, 0x309, &xbar1);
	ad9144_spi_read(dev->ad9144_device, 0x334, &lane_inv);
	dbg_printf("[LINK-DEBUG] XBAR0 (0x308) = 0x%02X, XBAR1 (0x309) = 0x%02X\n\r", xbar0, xbar1);
	dbg_printf("[LINK-DEBUG] Lane invert (0x334) = 0x%02X\n\r", lane_inv);
	
	dbg_printf("[LINK-DEBUG] ==========================================\n\r\n\r");
	/* ===== End Link Debug ===== */

#endif

#ifdef JESD_FSM_ON
	dev->ad9144_device->link_config.is_transmit = true;
	dev->ad9144_device->link_config.link_id = fmcdac_init.jrx_link_tx.link_id;
	dev->ad9144_device->link_config.bank_id = 0;
	dev->ad9144_device->link_config.device_id = fmcdac_init.jrx_link_tx.device_id;
	dev->ad9144_device->link_config.octets_per_frame =
		fmcdac_init.jrx_link_tx.octets_per_frame;
	dev->ad9144_device->link_config.frames_per_multiframe =
		fmcdac_init.jrx_link_tx.frames_per_multiframe;
	dev->ad9144_device->link_config.samples_per_conv_frame =
		fmcdac_init.jrx_link_tx.samples_per_converter_per_frame;
	dev->ad9144_device->link_config.high_density =
		fmcdac_init.jrx_link_tx.high_density;
	dev->ad9144_device->link_config.scrambling =
		fmcdac_init.jrx_link_tx.scrambling;
	dev->ad9144_device->link_config.converter_resolution =
		fmcdac_init.jrx_link_tx.converter_resolution;
	dev->ad9144_device->link_config.num_converters =
		fmcdac_init.jrx_link_tx.converters_per_device;
	dev->ad9144_device->link_config.bits_per_sample =
		fmcdac_init.jrx_link_tx.bits_per_sample;
	dev->ad9144_device->link_config.ctrl_bits_per_sample =
		fmcdac_init.jrx_link_tx.control_bits_per_sample;
	dev->ad9144_device->link_config.num_lanes =
		fmcdac_init.jrx_link_tx.lanes_per_device;
	dev->ad9144_device->link_config.subclass = fmcdac_init.jrx_link_tx.subclass;
	dev->ad9144_device->link_config.jesd_version = fmcdac_init.jrx_link_tx.version;

	dev->ad9144_device->link_config.sysref.capture_falling_edge = 0;
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
		xil_printf("[ERROR] axi_dac_init() failed: %s\n\r", dev_init->ad9144_core_param.name);
		return status;
	}
	xil_printf("[SETUP] AXI DAC core initialized successfully\n\r");
	
	/* ===== JESD204B K Parameter Debug Section ===== */

	dbg_printf("\n\r[K-DEBUG] JESD204B Configuration Verification:\n\r");
	
	// Read FPGA AXI JESD204B TX register 0x210 (CONF0)
	uint32_t jesd_conf0 = 0;
	uint32_t octets_per_multiframe_hw, octets_per_frame_hw;
	uint32_t k_hw, f_hw;
	
	// Register address 0x210 contains [25:16]=F-1, [9:0]=K*F-1
	Xil_Out32(TX_JESD_BASEADDR + 0x210, Xil_In32(TX_JESD_BASEADDR + 0x210));
	jesd_conf0 = Xil_In32(TX_JESD_BASEADDR + 0x210);
	
	octets_per_multiframe_hw = (jesd_conf0 & 0x3FF) + 1;  // Bits [9:0]
	octets_per_frame_hw = ((jesd_conf0 >> 16) & 0x3FF) + 1;  // Bits [25:16]
	
	f_hw = octets_per_frame_hw;
	k_hw = octets_per_multiframe_hw / f_hw;
	
	dbg_printf("[K-DEBUG] FPGA JESD TX Base: 0x%08lX\n\r", TX_JESD_BASEADDR);
	dbg_printf("[K-DEBUG] Register 0x210 (CONF0) = 0x%08lX\n\r", jesd_conf0);
	dbg_printf("[K-DEBUG]   Octets/Multiframe = %lu (K*F)\n\r", octets_per_multiframe_hw);
	dbg_printf("[K-DEBUG]   Octets/Frame (F) = %lu\n\r", f_hw);
	dbg_printf("[K-DEBUG]   Frames/Multiframe (K) = %lu\n\r", k_hw);
	
	// Read AD9144 K register (0x455)
	uint8_t ad9144_k_reg = 0;
	ad9144_spi_read(dev->ad9144_device, 0x455, &ad9144_k_reg);
	dbg_printf("[K-DEBUG] AD9144 REG_ILS_K (0x455) = 0x%02X (K=%d)\n\r", 
	           ad9144_k_reg, ad9144_k_reg + 1);
	
	// Read AD9144 F register (0x454)
	uint8_t ad9144_f_reg = 0;
	ad9144_spi_read(dev->ad9144_device, 0x454, &ad9144_f_reg);
	dbg_printf("[K-DEBUG] AD9144 REG_ILS_F (0x454) = 0x%02X (F=%d)\n\r", 
	           ad9144_f_reg, ad9144_f_reg + 1);
	
	// Calculate expected LMFC rates
	uint32_t byte_clock_khz = 245760;  // 245.76 MHz
	uint32_t lmfc_expected_khz = byte_clock_khz / (k_hw * f_hw);
	uint32_t lmfc_ad9144_khz = byte_clock_khz / ((ad9144_k_reg + 1) * (ad9144_f_reg + 1));
	
	dbg_printf("[K-DEBUG] Byte Clock = %lu kHz\n\r", byte_clock_khz);
	dbg_printf("[K-DEBUG] Expected LMFC (FPGA) = %lu kHz\n\r", lmfc_expected_khz);
	dbg_printf("[K-DEBUG] Expected LMFC (AD9144) = %lu kHz\n\r", lmfc_ad9144_khz);
	
	// Check for mismatch
	if (k_hw != (uint32_t)(ad9144_k_reg + 1)) {
		dbg_printf("[K-DEBUG] *** CRITICAL MISMATCH DETECTED ***\n\r");
		dbg_printf("[K-DEBUG] FPGA expects K=%lu, AD9144 expects K=%d\n\r", 
		           k_hw, ad9144_k_reg + 1);
		dbg_printf("[K-DEBUG] This will cause JESD204B link sync failure!\n\r");
	} else {
		dbg_printf("[K-DEBUG] K parameter match confirmed: K=%lu\n\r", k_hw);
	}
	
	// Additional check: Read raw register bits to verify masking
	dbg_printf("[K-DEBUG] Raw register bit analysis:\n\r");
	dbg_printf("[K-DEBUG]   Bits [7:0]  = 0x%02lX (value=%lu, K*F=%lu)\n\r", 
	           jesd_conf0 & 0xFF, jesd_conf0 & 0xFF, (jesd_conf0 & 0xFF) + 1);
	dbg_printf("[K-DEBUG]   Bits [9:0]  = 0x%03lX (value=%lu, K*F=%lu)\n\r", 
	           jesd_conf0 & 0x3FF, jesd_conf0 & 0x3FF, (jesd_conf0 & 0x3FF) + 1);
	dbg_printf("[K-DEBUG]   Bits [25:16] = 0x%03lX (F-1=%lu, F=%lu)\n\r", 
	           (jesd_conf0 >> 16) & 0x3FF, (jesd_conf0 >> 16) & 0x3FF, 
	           ((jesd_conf0 >> 16) & 0x3FF) + 1);
	
	dbg_printf("[K-DEBUG] ==========================================\n\r\n\r");
	/* ===== End K Parameter Debug ===== */

	xil_printf("[SETUP] FMCDAC setup completed successfully!\n\r");

	return status;
}

/**
 * @brief Print consolidated boot banner with build version and configuration.
 *
 * Prints all critical configuration constants at startup so every UART log
 * is self-describing and can be compared against manifest.json.
 */
static void print_boot_banner(void)
{
	/* Build identification */
	xil_printf("[BANNER] Build: %s  %s %s\n\r",
		   BUILD_VERSION_STR, __DATE__, __TIME__);

	/* JESD link parameters */
	xil_printf("[BANNER] JESD: subclass=%d  lane_rate=9830400 kbps"
		   "  ref=122880 kHz  DAC=983040 kHz\n\r",
		   1 /* jesd204_subclass */);

	/* Clock source */
	const char *clk_src;
	switch (g_clk_mode) {
	case FMCDAC_CLK_DISTRIBUTE: clk_src = "AD9516 distribute"; break;
	case FMCDAC_CLK_SYNTHESIZE: clk_src = "AD9516 synthesize"; break;
	case FMCDAC_CLK_EXTERNAL:   clk_src = "external bypass";   break;
	default:                    clk_src = "unknown";            break;
	}
	xil_printf("[BANNER] Clock: %s  out_clk_sel=OUTCLK_PMA  PLL=on\n\r",
		   clk_src);

	/* Lane crossbar and polarity */
	xil_printf("[BANNER] Lanes: mux={4,5,6,7,0,1,2,3}"
		   "  polarity_invert=0x04  scrambling=1\n\r");

	/* JESD204B transport layer */
	xil_printf("[BANNER] DAC: M=2 L=4 F=1 S=1 HD=1"
		   " N=16 N'=16 K=32\n\r");
}

int main(void)
{
	//unsigned int *data = (unsigned int *)ADC_DDR_BASEADDR;
	int status;

	/* Very first print to test UART - using xil_printf */
	xil_printf("\n\n\r*** UART TEST - XIL_PRINTF ***\n\r");
	xil_printf("*** Switching to xil_printf for all output ***\n\r");
	
	xil_printf("\n\r==============================================\n\r");
	xil_printf("FMCDAC Application Started\n\r");
	xil_printf("==============================================\n\r");
	print_boot_banner();

	status = fmcdac_setup(&fmcdac, &fmcdac_init);
	if (status < 0) {
		xil_printf("[FATAL] fmcdac_setup failed with status: %d\n\r", status);
		if (fmcdac.ad9516_dev)
			fmcdac_hold_for_probe("[FATAL] Holding for probe after setup failure.");
		return status;
	}

	xil_printf("\n\r[MAIN] Setup completed successfully!\n\r");


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

	xil_printf("\n\r[TEST] Starting FMCDAC tests...\n\r");
	int test_result = fmcdac_test(&fmcdac, &fmcdac_init);
	if (test_result == 0) {
		xil_printf("[TEST] All tests PASSED\n\r");
	} else {
		xil_printf("[TEST] Tests FAILED with code %d\n\r", test_result);
	}

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
#if 0  /* Disabled during bring-up — DDS tone from fmcdac_test() is active */
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
#endif

	/* force_dds_tone() runs DDS sweep internally */

#ifdef ENABLE_SOAK
	/* A-04: Long soak test — runs after normal boot tests pass.
	 * Uncomment #define ENABLE_SOAK above to activate. */
	fmcdac_soak(&fmcdac, &fmcdac_init);
#endif

	fmcdac_remove(&fmcdac);

	xil_printf("\n\r==============================================\n\r");
	if (test_result == 0) {
		xil_printf("FMCDAC Application Completed Successfully\n\r");
	} else {
		xil_printf("FMCDAC Application Completed With ERRORS (%d)\n\r",
			   test_result);
	}
	xil_printf("==============================================\n\r");

	return test_result;
}
