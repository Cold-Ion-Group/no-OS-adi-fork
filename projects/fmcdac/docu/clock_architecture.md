# Clock Architecture Reference — FMCDAC

**Board**: AD9144-FMC-EBZ mezzanine on KCU116 carrier
**Status**: Frozen per A-01 — changes require review
**Updated**: 2026-04-02

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
  │     │     Divider: ch0 /4 → 122.88 MHz
  │     │     Level: 960 mVpp diff
  │     │
  │     ├── OUT6 (LVDS ch0) ──── AD9144 SYSREF±  [DAC LMFC alignment]
  │     │     Divider: DIV3 configured by ad9516_frequency()
  │     │     Level: LVDS 3.5 mA (350 mVpp into 100 Ω)
  │     │     Freq: 30.72 MHz (= 491.52 / 16)
  │     │
  │     ├── OUT7 (LVDS ch1) ──── FPGA SYSREF (via FMC)  [TX LMFC alignment]
  │     │     Divider: DIV3 (same group as OUT6, shared divider)
  │     │     Level: LVDS 3.5 mA
  │     │     Freq: same as OUT6 (30.72 MHz)
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

## Frequency Table (Current Default: 1966 MSPS, 2× Interpolation)

| Signal | Frequency | Source | Destination | Impedance |
|--------|-----------|--------|-------------|-----------|
| Reference | 122.88 MHz | External SMA | AD9516 REF1 | 100 Ω diff |
| AD9516 VCO | 2457.6 MHz | Internal PLL | — | — |
| Post-VCO | 491.52 MHz | VCO ÷ 5 | Output dividers | — |
| DAC CLK (OUT1) | 122.88 MHz | Post-VCO ÷ 4 | AD9144 CLK± | 100 Ω LVPECL |
| DAC SYSREF (OUT6) | 30.72 MHz | Post-VCO ÷ 16 | AD9144 SYSREF± | 100 Ω LVDS |
| FPGA SYSREF (OUT7) | 30.72 MHz | Post-VCO ÷ 16 | FPGA via FMC | 100 Ω LVDS |
| FPGA REFCLK (OUT9) | 122.88 MHz | Post-VCO ÷ 4 (bypass) | GTH REFCLK | 100 Ω LVDS |
| AD9144 PLL ref in | 122.88 MHz | OUT1 | AD9144 PLL | — |
| AD9144 DAC clock | 1966.08 MHz | AD9144 PLL (×16) | Internal DAC core | — |
| AD9144 JESD input rate | 983.04 MSPS | JESD link | AD9144 deframer | — |
| JESD lane rate | 9830.4 Mbps | GTH QPLL0 | SerDes | 100 Ω diff |
| Device clock | 245.76 MHz | Lane rate ÷ 40 | JESD TX core | Internal |
| LMFC | 30.72 MHz | Device clk ÷ (K×F) | JESD framing | Internal |

> [!IMPORTANT]
> With **2× interpolation** (the current default), the AD9144 internal DAC
> clock runs at **1966.08 MHz** (PLL multiplier = ×16 from 122.88 MHz). The
> JESD link input rate remains 983.04 MSPS — the interpolation filter doubles
> the rate inside the AD9144. The FPGA side is completely unchanged vs the
> 1× mode.
>
> In 1× mode (compile-time option 2), the DAC PLL runs at 983.04 MHz (×8).

## AD9144 PLL Detail

| Parameter | 2× Interp (default) | 1× (option 2) |
|-----------|---------------------|----------------|
| PLL ref input | 122.88 MHz | 122.88 MHz |
| PLL multiplier | ×16 | ×8 |
| DAC clock output | 1966.08 MHz | 983.04 MHz |
| lo_div_mode | 1 (fdac ≥ 1500 MHz) | 2 (fdac ≥ 750 MHz) |
| VCO frequency | 7864.32 MHz | 7864.32 MHz |
| ref_div_mode | 1 | 0 |
| bcount | 16 | 8 |
| Interpolation | 2× | 1× (bypass) |
| Effective Nyquist | 983.04 MHz (DAC) / 491.52 MHz (FPGA DDS) | 491.52 MHz |

## GTH QPLL0 Detail

| Parameter | Value |
|-----------|-------|
| Reference clock | 122.88 MHz (from AD9516 OUT9) |
| Lane rate | 9830.4 Mbps |
| QPLL0 VCO | ~9.83 GHz |
| Sys clock select | QPLL0 |
| Out clock select | OUTCLK_PMA |
| Number of lanes | 4 (GTH TX) |
| LPM enable | 0 (DFE equalizer) |

## SYSREF Policy

| Parameter | Value | Rationale |
|-----------|-------|-----------|
| Init edge | **Rising** (0x04) | Hardcoded in `ad9144.c` driver; AD9144 aligns LMFC to rising edge |
| Runtime edge | **Rising** (0x04) | Tune skips when no alignment error; safety-net sweep available |
| Mode | Continuous | REG_SYNC_CTRL = 0xC1 (SYSREF-armed) |
| Frequency | fDAC / (K × S) | JESD204B §5.3.3.5 |
| Auto-tune | Safety net | `fmcdac_sysref_tune()` only activates if alignment error detected |

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
   b. DAC PLL lock (122.88 MHz → 1966.08 MHz with 2× interp)
   c. JESD link config (subclass 1, mode 4, lane mux)
   d. SYNC enable, SYSREF arm
8. JESD XCVR init      ← adxcvr_init() [QPLL0 lock]
9. JESD TX init        ← axi_jesd204_tx_init() [link enable]
10. JESD link-up       ← CGS → ILAS → DATA
11. SYSREF tune/verify ← fmcdac_sysref_tune() + fmcdac_sysref_verify()
12. Latency readback   ← fmcdac_latency_readback()
```

## Clock Domain Ownership

| Domain | Owner | Frequency | Notes |
|--------|-------|-----------|-------|
| DAC sample clock | AD9144 PLL | 1966.08 MHz (2×) or 983.04 MHz (1×) | From 122.88 MHz ref |
| JESD TX parallel | GTH QPLL0 | 245.76 MHz | = lane_rate / 40 |
| GTH serial | GTH QPLL0 | 9.8304 GHz | 4 lanes |
| LMFC (DAC side) | AD9144 | 30.72 MHz | Aligned by SYSREF |
| LMFC (FPGA side) | AXI JESD TX | 30.72 MHz | Aligned by SYSREF |
| AXI/CPU | MicroBlaze | 100 MHz | System clock |
| SPI | AXI SPI | ≤ 50 MHz | Register access |

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

---

## Future Work: Atomic Reference (5/10 MHz) Clock Path

### Goal

Replace the current 122.88 MHz external reference with a **5 MHz or 10 MHz
atomic frequency standard** (e.g., Rb or GPS-disciplined OCXO) to achieve
long-term frequency accuracy and stability traceable to an atomic reference.

### Problem

The AD9516-1 PLL reference input (REF1) requires a **minimum of 35 MHz** and
a **maximum of 250 MHz** for its internal PLL to lock. A 5 MHz or 10 MHz
reference cannot be fed directly to the AD9516.

### Required Architecture

An **external PLL/synthesizer** must be inserted between the atomic reference
and the AD9516 REF1 input:

```
5/10 MHz Atomic Ref
      │
      ▼
┌─────────────────────┐
│  External PLL       │
│  (e.g., ADF4351,    │
│   Si5351A, LMX2594, │
│   or similar)       │
│                     │
│  Input: 5/10 MHz    │
│  Output: 122.88 MHz │
│  differential       │
└─────────┬───────────┘
          │ 122.88 MHz
          ▼
     AD9516-1 REF1
     (rest of clock tree unchanged)
```

### External PLL Options

| Option | Device | Input Range | Output | Phase Noise | Notes |
|--------|--------|-------------|--------|-------------|-------|
| A | ADF4351 | 10 MHz–6 GHz | 122.88 MHz | –95 dBc/Hz @1kHz | Integer-N possible (12288 = 10 × 1228.8) |
| B | LMX2594 | 5 MHz–15 GHz | 122.88 MHz | –110 dBc/Hz @1kHz | Higher performance, more complex |
| C | Si5351A | 10–40 MHz XTAL/CLKIN | 122.88 MHz | –130 dBc/Hz @1kHz (typ) | I2C programmed, cheap, moderate jitter |
| D | AD9520/AD9523 | 1 MHz–1 GHz | 122.88 MHz | –100 dBc/Hz @1kHz | ADI ecosystem, same SPI style as AD9516 |

### Integer-N Multiplication

For a clean 122.88 MHz from common atomic references:

| Reference | Multiplier | Result | Clean integer? |
|-----------|-----------|--------|----------------|
| 10 MHz | × 12.288 | 122.88 MHz | No — fractional, requires fractional-N PLL |
| 10 MHz | × 12 → 120 MHz | 120 MHz | Yes — but 120 ≠ 122.88. Would require recalculating all downstream dividers |
| 5 MHz | × 24.576 | 122.88 MHz | No — fractional |

> [!WARNING]
> **122.88 MHz is not an integer multiple of 5 or 10 MHz.** Any external PLL
> from a standard atomic reference must use **fractional-N synthesis** to
> produce 122.88 MHz, which adds fractional spur risk. The alternative is to
> redesign the downstream frequency plan to use a clean integer-derivable
> frequency (e.g., 120 MHz or 125 MHz), but this would require changes to:
>
> - AD9516 VCO/divider programming
> - AD9144 PLL reference configuration
> - JESD lane rate and device clock
> - SYSREF/LMFC frequencies
> - FPGA QPLL0 reference and constraints

### Recommended Approach

1. **Short term**: Use a fractional-N PLL (e.g., ADF4351 or LMX2594) to
   synthesize 122.88 MHz from 10 MHz. Accept the fractional spur floor
   (typically < –70 dBc, which is below the current SFDR baseline anyway).
   The rest of the clock tree remains completely unchanged.

2. **Long term (if spur floor matters)**: Evaluate whether the frequency plan
   can be redesigned around a clean integer reference. This is a significant
   architectural change that touches AD9516 dividers, AD9144 PLL, JESD lane
   rate, and FPGA constraints. Only pursue if the fractional spur floor becomes
   a limiting factor after SFDR is improved to >70 dBc.

### Integration Points

| Item | Change Required | Risk |
|------|-----------------|------|
| External PLL board/module | New hardware (SMA-to-SMA or small PCB) | Low |
| AD9516 REF1 input | No change — still receives 122.88 MHz | None |
| AD9144 PLL | No change | None |
| FPGA GTH REFCLK | No change | None |
| SYSREF | No change | None |
| Firmware | No change (unless external PLL needs SPI/I2C programming) | Low |
| Phase noise budget | External PLL adds to chain — must verify total at AD9144 CLK | Medium |
| Spur budget | Fractional-N spurs add to spectral floor — measure | Medium |

### Verification Plan

1. Measure phase noise at AD9144 CLK input with external PLL vs direct reference
2. Run full DDS-band and SFDR comparison (same firmware, same analyzer settings)
3. Check for new spurs at fractional-N spur offsets
4. Verify AD9516 PLL lock with the new reference source
5. Confirm JESD link integrity (CGS/ILAS/DATA all still 0x0F)
