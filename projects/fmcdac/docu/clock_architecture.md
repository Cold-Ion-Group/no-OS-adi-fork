# Clock Architecture Reference — FMCDAC

**Board**: AD9144-FMC-EBZ mezzanine on KCU116 carrier
**Status**: Frozen per A-01 — changes require review

---

## Clock Tree

```
External 122.88 MHz differential
  │
  ├── AD9516-1 REF1 (CLK/CLK_N)
  │     PLL: R=1, N=20, VCO = 2457.6 MHz, VCO divider = /5
  │     Post-VCO = 491.52 MHz
  │     │
  │     ├── OUT1 (LVPECL ch0) ─── AD9144 CLK±  [DAC PLL reference]
  │     │     Divider: ch0 /4 → 122.88 MHz (mode-dependent, see table)
  │     │     Level: 960 mVpp diff
  │     │
  │     ├── OUT6 (LVDS ch0) ──── AD9144 SYSREF±  [DAC LMFC alignment]
  │     │     Divider: DIV3 configured by ad9516_frequency()
  │     │     Level: LVDS 3.5 mA (350 mVpp into 100 Ω)
  │     │     Freq: fDAC/(K×S) = 30.72 MHz @ 983 MSPS mode 4
  │     │
  │     ├── OUT7 (LVDS ch1) ──── FPGA SYSREF (via FMC)  [TX LMFC alignment]
  │     │     Divider: DIV3 (same group as OUT6, shared divider)
  │     │     Level: LVDS 3.5 mA
  │     │     Freq: same as OUT6
  │     │
  │     ├── OUT8 (LVDS ch2) ──── unused (powered down)
  │     │
  │     └── OUT9 (LVDS ch3) ──── FPGA GTH REFCLK  [QPLL0 reference]
  │           Divider: DIV4 BYPASSED → 122.88 MHz (from post-VCO ÷ 4)
  │           Level: LVDS 3.5 mA
  │
  └── Si5328 (BYPASSED — #define SKIP_SI5328)
        Was: 114.285 MHz → 245.76 MHz for GTH REFCLK
        Now: AD9516 OUT9 provides REFCLK directly
```

## Frequency Table (983 MSPS Default)

| Signal | Frequency | Source | Destination | Impedance |
|--------|-----------|--------|-------------|-----------|
| Reference | 122.88 MHz | External SMA | AD9516 REF1 | 100 Ω diff |
| AD9516 VCO | 2457.6 MHz | Internal | — | — |
| Post-VCO | 491.52 MHz | VCO ÷ 5 | Output dividers | — |
| DAC CLK (OUT1) | 122.88 MHz | Post-VCO ÷ 4 | AD9144 CLK± | 100 Ω LVPECL |
| DAC SYSREF (OUT6) | 30.72 MHz | Post-VCO ÷ 16 | AD9144 SYSREF± | 100 Ω LVDS |
| FPGA SYSREF (OUT7) | 30.72 MHz | Post-VCO ÷ 16 | FPGA via FMC | 100 Ω LVDS |
| FPGA REFCLK (OUT9) | 122.88 MHz | Post-VCO ÷ 4 (bypass) | GTH REFCLK | 100 Ω LVDS |
| AD9144 DAC clock | 983.04 MHz | AD9144 PLL (×8) | Internal | — |
| JESD lane rate | 9830.4 MHz | GTH QPLL0 | SerDes | 100 Ω diff |
| Device clock | 245.76 MHz | Lane rate ÷ 40 | JESD TX core | Internal |
| LMFC | 30.72 MHz | Device clk ÷ (K×F) | JESD framing | Internal |

## SYSREF Policy

| Parameter | Value | Rationale |
|-----------|-------|-----------|
| Init edge | **Rising** (0x04) | Hardcoded in `ad9144.c` driver; AD9144 aligns LMFC to rising edge |
| Runtime edge | **Rising** (0x04) | Tune skips when no alignment error; safety-net sweep available |
| Mode | Continuous | REG_SYNC_CTRL = 0xC1 (SYSREF-armed) |
| Frequency | fDAC / (K × S) | JESD204B §5.3.3.5 |
| Auto-tune | Safety net | `fmcdac_sysref_tune()` only activates if alignment error detected |

## Clock Domain Ownership

| Domain | Owner | Frequency | Notes |
|--------|-------|-----------|-------|
| DAC sample clock | AD9144 PLL | 983.04 MHz | From 122.88 MHz ref |
| JESD TX parallel | GTH QPLL0 | 245.76 MHz | = lane_rate / 40 |
| GTH serial | GTH QPLL0 | 9.8304 GHz | 4 lanes |
| LMFC (DAC side) | AD9144 | 30.72 MHz | Aligned by SYSREF |
| LMFC (FPGA side) | AXI JESD TX | 30.72 MHz | Aligned by SYSREF |
| AXI/CPU | MicroBlaze | 100 MHz | System clock |
| SPI | AXI SPI | ≤ 50 MHz | Register access |

## Startup Sequence

```
1. GPIO init           ← fmcdac_gpio_init()
2. SPI init            ← fmcdac_spi_init()
3. I2C init            ← fmcdac_i2c_init()
4. [Si5328 setup]      ← SKIPPED (#define SKIP_SI5328)
5. AD9516 PLL init     ← ad9516_setup() [VCO lock]
6. AD9516 outputs      ← fmcdac_ad9516_program_outputs()
   a. Set DAC CLK frequency (OUT1 via LVPECL dividers)
   b. Set SYSREF frequency (OUT6/7 via ad9516_frequency())
   c. Set FPGA REFCLK (OUT9 via DIV4 bypass)
   d. Power up all outputs, update registers
7. AD9144 init         ← ad9144_setup()
   a. Power up, write required config, SYSREF buffer on (rising edge SYSREF_RISE)
   b. DAC PLL lock (122.88 MHz → 983.04 MHz)
   c. JESD link config (subclass 1, mode 4, lane mux)
   d. SYNC enable, SYSREF arm
8. JESD XCVR init      ← adxcvr_init() [QPLL0 lock]
9. JESD TX init        ← axi_jesd204_tx_init() [link enable]
10. JESD link-up       ← CGS → ILAS → DATA
11. SYSREF tune/verify ← fmcdac_sysref_tune() + fmcdac_sysref_verify()
12. Latency readback   ← fmcdac_latency_readback()
```

## AD9516 Register Settings (Key)

| Register | Value | Description |
|----------|-------|-------------|
| REF1 freq | 122,880,000 Hz | `ref_1_freq` |
| VCO freq | 2,457,600,000 Hz | `int_vco_freq` |
| VCO divider | /5 | Implicit from `vco_clk_sel=1` |
| OUT1 level | LVPECL 960 mV | `LVPECL_960mV` |
| OUT6/7 level | LVDS 3.5 mA | `LVDS_3_5mA` |
| OUT9 level | LVDS 3.5 mA | `LVDS_3_5mA` |
| DIV3 (OUT6/7) | Programmable | Set by `ad9516_frequency()` for SYSREF target |
| DIV4 (OUT8/9) | Bypassed | Forced bypass for 122.88 MHz REFCLK |

## FMC Pin Mapping

| Signal | AD9516 Output | FMC Pin | Direction |
|--------|---------------|---------|-----------|
| DAC CLK± | OUT1 (LVPECL) | Board-direct (mezzanine) | AD9516 → AD9144 |
| DAC SYSREF± | OUT6 (LVDS) | Board-direct (mezzanine) | AD9516 → AD9144 |
| FPGA SYSREF± | OUT7 (LVDS) | FMC LA03_P/N (verify) | AD9516 → FPGA |
| FPGA REFCLK± | OUT9 (LVDS) | FMC CLK0/1_M2C | AD9516 → FPGA GTH |
| SYNC~ | AD9144 | FMC pin (verify) | AD9144 → FPGA |
