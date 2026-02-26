// configuration file for the ad9516 clock chip, it does not contain ports to the in 
#ifndef __AD9516_CFG_H__
#define __AD9516_CFG_H__
// 40 mhz is the external clock reference frequency from the AWG 
struct ad9516_platform_data ad9516_pdata_lpc = {
	/* PLL Reference */
	0, // ref_1_freq
    0, // ref_2_freq
	1, // diff_ref_en
	1, // ref_1_power_on
	1, // ref_2_power_on
	0, // ref_sel_pin_en
	1, // ref_sel_pin
	0, // ref_2_en

	40000000, // ext_clk_freq
	1600000000, // int_vco_freq
	0, // vco_clk_sel
	0, // power_down_vco_clk
	"ad9516_channels" // name[16]
};

// 6 LVPECL output, 4  of lvds clock output 

struct ad9516_lvpecl_channel_spec ad9516_lvpecl_channels[] = {
	{
		0, // channel_num - Output channel number.
		0, // out_invert_en - Invert the polarity of the output clock.l
		LVPECL_780mV, // out_diff_voltage - LVPECL output differential voltage.
		"CH0" // name[16] - Optional descriptive channel name.
	},
	{
		1, // channel_num - Output channel number.
		0, // out_invert_en - Invert the polarity of the output clock.
		LVPECL_780mV, // out_diff_voltage - LVPECL output differential voltage.
		"CH1" // name[16] - Optional descriptive channel name.
	},
	{
		2, // channel_num - Output channel number.
		0, // out_invert_en - Invert the polarity of the output clock.
		LVPECL_780mV, // out_diff_voltage - LVPECL output differential voltage.
		"CH2" // name[16] - Optional descriptive channel name.
	},
	{
		3, // channel_num - Output channel number.
		0, // out_invert_en - Invert the polarity of the output clock.
		LVPECL_960mV, // out_diff_voltage - LVPECL output differential voltage.
		"CH3" // name[16] - Optional descriptive channel name.
	},
		{
		4, // channel_num - Output channel number.
		0, // out_invert_en - Invert the polarity of the output clock.
		LVPECL_780mV, // out_diff_voltage - LVPECL output differential voltage.
		"CH4" // name[16] - Optional descriptive channel name.
	},
	{
		5, // channel_num - Output channel number.
		0, // out_invert_en - Invert the polarity of the output clock.
		LVPECL_780mV, // out_diff_voltage - LVPECL output differential voltage.
		"CH5" // name[16] - Optional descriptive channel name.
	},
	{
		6, // channel_num - Output channel number.
		0, // out_invert_en - Invert the polarity of the output clock.
		LVPECL_960mV, // out_diff_voltage - LVPECL output differential voltage.
		"CH6" // name[16] - Optional descriptive channel name.
	}
};

struct ad9516_lvds_cmos_channel_spec ad9516_lvds_cmos_channels[] = {
	{
		7, // channel_num - Output channel number.
		0, // out_invert
		LVDS, // logic_level - Select LVDS or CMOS logic levels.
		0, // cmos_b_en - In CMOS mode, turn on/off the CMOS B output.
		LVDS_3_5mA, // out_lvds_current - LVDS output current level.
		"CH7" // name[16] - Optional descriptive channel name.
	},
	{
		8, // channel_num - Output channel number.
		0, // out_invert
		LVDS, // logic_level - Select LVDS or CMOS logic levels.
		0, // cmos_b_en - In CMOS mode, turn on/off the CMOS B output.
		LVDS_3_5mA, // out_lvds_current - LVDS output current level.
		"CH8" // name[16] - Optional descriptive channel name.
	},
	{
		9, // channel_num - Output channel number.
		0, // out_invert
		LVDS, // logic_level - Select LVDS or CMOS logic levels.
		1, // cmos_b_en - In CMOS mode, turn on/off the CMOS B output.
		LVDS_3_5mA, // out_lvds_current - LVDS output current level.
		"CH9" // name[16] - Optional descriptive channel name.
	},
	{
		10, // channel_num - Output channel number.
		0, // out_invert
		LVDS, // logic_level - Select LVDS or CMOS logic levels.
		0, // cmos_b_en - In CMOS mode, turn on/off the CMOS B output.
		LVDS_3_5mA, // out_lvds_current - LVDS output current level.
		"CH10" // name[16] - Optional descriptive channel name.
	}
};

#endif // __AD9516_CFG_H__
