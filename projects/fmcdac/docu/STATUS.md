# FMCDAC System Status Report

**Date**: 2026-02-26
**Platform**: AD9144-FMC-EBZ mezzanine on KCU116 (MicroBlaze)
**Toolchain**: Vivado/Vitis 2021.2

---

## System Design

### Hardware Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│  KCU116 Evaluation Board (Kintex UltraScale XCKU5P)             │
│                                                                  │
│  ┌──────────┐    AXI     ┌──────────┐   JTAG    ┌─────────┐    │
│  │MicroBlaze├────────────┤ AXI SPI  ├───────────►│ SPI bus │    │
│  │ (100 MHz)│            └──────────┘            └────┬────┘    │
│  │          │    AXI     ┌──────────────┐             │         │
│  │          ├────────────┤ AXI JESD204  │             │         │
│  │          │            │  TX (ADI IP) │             │         │
│  │          │            └──────┬───────┘             │         │
│  │          │    AXI     ┌──────┴───────┐             │         │
│  │          ├────────────┤  AXI DAC TPL │             │         │
│  │          │            │  (ADI IP)    │             │         │
│  └──────────┘            └──────┬───────┘             │         │
│                                 │ 4× GTH              │         │
│                          ┌──────┴───────┐             │         │
│                          │  GTH XCVR    │             │         │
│                          │  (QPLL0)     │             │         │
│                          └──────┬───────┘             │         │
│                                 │ 4 lanes             │         │
│  FMC HPC ───────────────────────┼─────────────────────┤         │
└─────────────────────────────────┼─────────────────────┼─────────┘
                                  │                     │
┌─────────────────────────────────┼─────────────────────┼─────────┐
│  AD9144-FMC-EBZ Mezzanine       │                     │         │
│                                 │ JESD204B            │ SPI     │
│                          ┌──────┴───────┐      ┌──────┴──────┐  │
│                          │   AD9144     │      │   AD9516-1  │  │
│                          │  Quad DAC    │      │  Clock Dist │  │
│                          │  16-bit      │◄─CLK─┤  (PLL/VCO)  │  │
│                          │  2.8 GSPS    │◄SYSREF┤             │  │
│                          └──────┬───────┘      └──────┬──────┘  │
│                                 │ 2× analog           │         │
│                          ┌──────┴───────┐      ┌──────┴──────┐  │
│                          │  DAC0  DAC1  │      │ 122.88 MHz  │  │
│                          │  (I)   (Q)   │      │  ext. ref   │  │
│                          └──────────────┘      └─────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

### FPGA Block Design

| IP Core | Base Address | Function |
|---------|-------------|----------|
| MicroBlaze | — | Processor (100 MHz, v11.0, little-endian) |
| AXI SPI | 0x44A70000 | SPI master to AD9516 (CS0) + AD9144 (CS1) |
| AXI GPIO | 0x40000000 | DAC_RESET, DAC_TXEN, CLKD_SYNC, DAC_CTRL[1:0] |
| AXI IIC | 0x41600000 | I2C to Si5328 (via mux at 0x74, bypassed) |
| AXI JESD204 TX | 0x44A90000 | JESD204B TX link layer (ADI IP, Subclass 1) |
| AXI AD9144 TPL | 0x44A04000 | Transport + DDS core (2 channels, DPW=4) |
| AXI ADXCVR | 0x44A60000 | GTH transceiver control (QPLL0, 4 lanes) |
| AXI DMAC | 0x7C420000 | DMA for waveform playback from DDR |
| AXI UART | — | MicroBlaze console (115200 8N1) |

### Firmware Architecture

```
main()
  └── fmcdac_reconfig()          ← Interactive: clock mode + sampling rate menu
  └── fmcdac_setup()
        ├── fmcdac_gpio_init()    ← DAC_RESET, DAC_TXEN, CLKD_SYNC pins
        ├── fmcdac_spi_init()     ← AXI SPI: CS0=AD9516, CS1=AD9144
        ├── fmcdac_i2c_init()     ← AXI IIC (Si5328 path, bypassed)
        ├── ad9516_setup()        ← PLL lock, output frequency programming
        ├── fmcdac_ad9516_program_outputs()
        │     ├── DAC CLK (OUT1)  ← 122.88 MHz LVPECL
        │     ├── SYSREF (OUT6/7) ← 30.72 MHz LVDS
        │     └── REFCLK (OUT9)   ← 122.88 MHz LVDS (bypass)
        ├── adxcvr_init()         ← QPLL0 lock, lane rate 9.83 Gbps
        ├── ad9144_setup()        ← DAC PLL, JESD config, SYSREF, XBAR
        │     ├── Power-up (SYSREF_RISE = rising edge default)
        │     ├── DAC PLL lock (122.88 → 983.04 MHz)
        │     ├── JESD link config (subclass=1, mode 4, lane mux)
        │     └── SYNC enable, SYSREF arm
        ├── axi_jesd204_tx_init() ← Link enable → CGS → ILAS → DATA
        └── axi_dac_init()        ← Transport layer + DDS core
  └── fmcdac_test()
        ├── JESD status + link poll
        ├── STPL zero test         ← DDS scale=0 for guaranteed zeros
        ├── STPL pattern test      ← SED mode, 4 samples per DAC
        ├── PRBS-7 test            ← PN7 source → AD9144 PRBS checker
        ├── PRBS-15 test           ← PN15 source → AD9144 PRBS checker
        ├── fmcdac_sysref_tune()   ← Safety net (skips if no error)
        ├── fmcdac_sysref_verify() ← Subclass 1 register dump
        ├── fmcdac_latency_readback() ← dyn0/dyn1/var0/var1
        ├── fmcdac_phy_prbs_test() ← PHY-level PRBS (void, observability-only)
        └── force_dds_tone()       ← DDS sweep 10–200 MHz
  └── [fmcdac_soak()]             ← Optional (ENABLE_SOAK), 8h link monitor
  └── fmcdac_remove()             ← Teardown
```

### JESD204B Link Parameters (Mode 4)

| Parameter | Symbol | Value | Description |
|-----------|--------|-------|-------------|
| Converters | M | 2 | DAC0 (I) + DAC1 (Q) |
| Lanes | L | 4 | GTH lanes 0–3 |
| Octets/frame | F | 1 | Minimum framing |
| Samples/converter/frame | S | 1 | Single sample per frame |
| Bits/sample | N | 16 | Full resolution |
| Total bits/sample | N' | 16 | No padding |
| Frames/multiframe | K | 32 | K×F = 32 octets/multiframe |
| High-density | HD | 1 | Enabled (required for F=1) |
| Scrambling | SCR | 1 | 8B/10B scrambling enabled |
| Subclass | — | 1 | Deterministic latency (SYSREF-aligned) |
| Lane rate | — | 9830.4 Mbps | = M×S×N'×10/8 × fDAC / L |
| Device clock | — | 245.76 MHz | = lane_rate / 40 |
| LMFC | — | 30.72 MHz | = device_clk / (K×F) |

### Lane Crossbar

The physical lane wiring on the AD9144-FMC-EBZ does not match the logical lane
order expected by the AD9144. The firmware configures the AD9144's receive XBAR
registers to correct this:

| Logical Lane | Physical Lane | XBAR Register |
|-------------|---------------|---------------|
| 0 | 4 | 0x308 = 0x2C |
| 1 | 5 | |
| 2 | 6 | 0x309 = 0x3E |
| 3 | 7 | |

Lane 2 has polarity inversion (`REG_PHY_PRBS_TEST_CTRL (0x334) = 0x04`).

---

## Executive Summary

The JESD204B data path is **fully functional** at 983 MSPS with Subclass 1 enabled.
All digital-layer tests pass consistently. SYSREF alignment works from init with
rising edge hardcoded in the driver. DDS tone generation and frequency sweep are
operational. Deterministic latency has **not yet been verified** across multiple
boots — the infrastructure is built but multi-boot testing is pending.

---

## What Works (Verified by Boot Log)

### JESD204B Link — PASS

| Parameter | Value | Status |
|-----------|-------|--------|
| Mode | 4 (M=2 L=4 F=1 S=1 HD=1 K=32) | ✅ |
| Lane rate | 9830.400 MHz (4 lanes) | ✅ |
| Device clock | 245.760 MHz (measured 245.764) | ✅ |
| LMFC rate | 30.720 MHz | ✅ |
| Link state | DATA | ✅ |
| SYNC~ | Deasserted | ✅ |
| CGS (all 4 lanes) | 0x0F | ✅ |
| Frame sync (all 4 lanes) | 0x0F | ✅ |
| Checksum (all 4 lanes) | 0x0F | ✅ |
| ILAS sync (all 4 lanes) | 0x0F | ✅ |
| Scrambling | Enabled (SCR=1) | ✅ |

The link comes up on first attempt (poll=0) with zero retries.

### Subclass 1 — PASS

| Check | Value | Status |
|-------|-------|--------|
| JRX_CTRL_1 (0x301) | 0x01 (subclass=1) | ✅ |
| ILAS SUBCLASSV | 1 (ILS_NP 0x458 = 0x2F) | ✅ |
| SYSREF captured (FPGA TX) | Yes | ✅ |
| SYSREF alignment error (FPGA TX) | None (tune skipped) | ✅ |
| SYSREF_ACTRL0 final | 0x04 (rising edge, powered up) | ✅ |
| SYNC_CTRL (0x03A) | 0x81 (SYSREF-armed) | ✅ |
| TX SYSREF_STATUS (0x108) | 0x01 (captured, no error) | ✅ |

**SYSREF edge**: Rising edge is hardcoded in the AD9144 driver init (`SYSREF_RISE`, 0x04).
With this default, `fmcdac_sysref_tune()` sees no alignment error and skips. The tune
sweep remains as a safety net — if an error does appear, it tries the opposite edge and
LMFC offset sweep. Starting with falling edge from driver init was validated to permanently
brick SYSREF alignment.

### Data Path Integrity — PASS

| Test | Result |
|------|--------|
| STPL zero pattern (DAC0 + DAC1) | PASS |
| STPL data pattern DAC0 (4 samples: 0xA1A0, 0xB1B0 × 2) | PASS |
| STPL data pattern DAC1 (4 samples: 0xC1C0, 0xD1D0 × 2) | PASS |
| PRBS-7 (all lanes) | PASS |
| PRBS-15 (all lanes) | PASS |

The short transport layer pattern test validates the entire FPGA → SerDes → AD9144
data path, including lane crossbar (XBAR), lane inversion (lane 2), and byte
deframing. PRBS tests confirm pseudo-random data survives the full link.

### Clock Distribution — PASS

| Signal | Target | Actual | Status |
|--------|--------|--------|--------|
| External ref → AD9516 REF1 | 122.88 MHz | 122.88 MHz | ✅ |
| AD9516 OUT1 → AD9144 CLK | 122.88 MHz | 122.88 MHz | ✅ |
| AD9516 OUT6 → AD9144 SYSREF | 30.72 MHz | 30.72 MHz | ✅ |
| AD9516 OUT7 → FPGA SYSREF | 30.72 MHz | 30.72 MHz | ✅ |
| AD9516 OUT9 → FPGA GTH REFCLK | 122.88 MHz | 122.88 MHz | ✅ |
| AD9144 DAC PLL | 983.04 MHz | 983.057 MHz | ✅ |
| AD9144 PLL status (0x084) | 0x22 (locked) | 0x22 | ✅ |
| SERDES PLL status (0x281) | locked | 0x0B (locked=1) | ✅ |

Si5328 is bypassed (`SKIP_SI5328`). GTH REFCLK comes directly from AD9516 OUT9.

### DDS Tone Generation — PASS

| Parameter | Value |
|-----------|-------|
| Core clock | 983.057 MHz |
| DAC full-scale current | 0x01FF (max) |
| Frequency sweep | 10 → 200 MHz in 5 MHz steps (39 points) |
| Sweep hold time | 50 ms per step |
| All sweep steps | Completed without link drop |

---

## Known Issues

### PHY-Level PRBS — Observability Only (expected failure)

```
[PHY-PRBS] test_en=0x0F ctrl=0x02 status=0xF0 err_count=16777215
[PHY-PRBS] FAILED: 16777215 errors detected
```

This is **expected and benign**. `fmcdac_phy_prbs_test()` is `void` and does not
participate in pass/fail accounting. The PHY-PRBS checker requires the FPGA TX to
generate raw PHY-level PRBS patterns (bypassing JESD framing), which requires HDL
changes. The error counter saturates at 2²⁴−1. The datapath-level PRBS7/PRBS15
tests validate data integrity through the full framing stack.

### Latency Stability — PARTIALLY CHARACTERIZED

Three boot samples:

| Boot | dyn0 | dyn1 | var0 | var1 |
|------|------|------|------|------|
| Boot 1 (earlier session) | 0x03 | 0x03 | 0x0A | 0x0A |
| Boot 2 (Subclass 1 tune) | 0x03 | 0x03 | 0x0A | 0x0A |
| Boot 3 (earlier, pre-sc1) | 0x02 | 0x02 | 0x0A | 0x0A |

The two Subclass 1 boots show identical latency (dyn=0x03). The earlier Subclass 0
boot showed dyn=0x02. `var0`/`var1` are always 0x0A (programmed constant).

**Status**: Infrastructure to characterize this is built (`verify_latency.ps1`).
5+ boot captures are needed to confirm deterministic latency.

---

## What Has Not Been Tested

| Item | Description | Blocked By |
|------|-------------|------------|
| **A-03: Multi-boot latency** | 5+ boot-cycle comparison of dyn0/dyn1 | Automation script or manual TeraTerm captures |
| **A-04: Link soak (8h+)** | Long-duration stability with periodic PRBS checks | Requires overnight board time |
| **A-05: Clock margin sweep** | AD9516 output level / refclk tolerance sweep | Lab equipment + board time |
| **Scope timing (CLK-4)** | tSSD / tHSD at AD9144 SYSREF/CLK pins | Scope with ≥2 GHz BW |
| **Multi-mode** | Modes other than mode 4 (500 MSPS, 600 MSPS, 2 GSPS) | Not attempted |
| **Temperature** | Operation across temperature range | Environmental chamber |
| **Analog output quality** | Spurious-free dynamic range, SFDR, SNR | Spectrum analyzer |

---

## File Inventory

### Firmware (modified)

| File | Changes |
|------|---------|
| `drivers/dac/ad9144/ad9144.c` | Subclass 1 init (`SYSREF_RISE` default in both init paths) |
| `projects/fmcdac/src/app/fmcdac.c` | SYSREF tune/verify, latency readback, soak loop, DDS sweep, STPL, PRBS, PHY-PRBS (void), JESD_FSM_ON naming fix (`jrx_link_tx`) |
| `projects/fmcdac/src/app/parameters.h` | AD9516 output index defines, FMC pin mapping enum |

### Documentation

| File | Contents |
|------|----------|
| `projects/fmcdac/SYSTEM_OVERVIEW.md` | 2-page system overview, data flow, capabilities |
| `projects/fmcdac/clock_architecture.md` | Clock tree, frequency table, SYSREF policy, startup sequence |

### Test Infrastructure

| File | Purpose | Status |
|------|---------|--------|
| `projects/fmcdac/verify_uart.ps1` | Single-boot pass/fail (13 criteria) | Working |
| `projects/fmcdac/verify_latency.ps1` | Multi-boot latency fingerprint comparison | Written, untested at scale |
| `projects/fmcdac/run_test.py` | Full automation (build/flash/capture/verify loop) | Serial capture not working |
| `projects/fmcdac/capture_uart.ps1` | PowerShell serial capture (alternative) | Written, untested |
| `projects/fmcdac/test_serial.py` | pyserial COM port diagnostic | Written |

### Build & Workflow

| File | Purpose |
|------|---------|
| `projects/fmcdac/Makefile` | Build with manifest verification |
| `projects/fmcdac/gen_manifest.ps1` | XSA + firmware commit tracking |
| `.agent/workflows/verify.md` | `/verify` workflow (build → flash → capture → verify) |

---

## Recommended Next Steps (Priority Order)

1. **Diagnose serial capture** — run `python test_serial.py` to determine if
   pyserial can read COM4. If not, try with DTR enabled or check alternative COM ports.

2. **Manual multi-boot test** — capture 5 boot logs via TeraTerm, run
   `.\verify_latency.ps1 -LogDir .\latency_logs\` to characterize latency stability.

3. **If latency varies** — investigate SYSREF timing margin at the scope (tSSD/tHSD).
   Consider adjusting `LMFC_OFFSET` in the FPGA TX core.

4. **If latency is stable** — run 8h soak test (uncomment `ENABLE_SOAK`, rebuild, run overnight).
