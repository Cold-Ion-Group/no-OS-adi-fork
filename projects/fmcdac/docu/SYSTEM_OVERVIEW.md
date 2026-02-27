# FMCDAC System Overview

**Platform**: AD9144-FMC-EBZ on Xilinx KCU116 (Kintex UltraScale XCKU5P)
**DAC**: AD9144 — dual 16-bit, 983 MSPS, JESD204B Subclass 1
**Processor**: MicroBlaze soft-core (100 MHz, bare-metal)

---

## 1. Hardware

An external 122.88 MHz reference clock feeds the AD9516-1 clock distributor on the
FMC mezzanine. The AD9516 fans out three signals:

| Output | Signal | Frequency | Destination |
|--------|--------|-----------|-------------|
| OUT1 | DAC CLK | 122.88 MHz | AD9144 PLL reference → 983.04 MHz sample clock |
| OUT6/7 | SYSREF | 30.72 MHz | AD9144 + FPGA (LMFC alignment) |
| OUT9 | REFCLK | 122.88 MHz | FPGA GTH QPLL0 → 9.83 Gbps serial lane rate |

The FPGA connects to the AD9144 over four GTH transceiver lanes carrying JESD204B
traffic. MicroBlaze controls the AD9516 and AD9144 via SPI (directly — no Linux,
no OS).

## 2. Data Flow

```
 MicroBlaze (firmware)
       │
       ├── SPI ──────────────► AD9516  (clock programming)
       ├── SPI ──────────────► AD9144  (DAC config, JESD link control)
       │
       ├── AXI ──► AXI DAC TPL ──► AXI JESD TX ──► GTH TX ──► AD9144 JESD RX
       │           (DDS engine)     (link layer)    (SerDes)    (deframer)
       │           2 ch, DPW=4      SC1, K=32       4 lanes     ──► DAC0/1
       │                                            9.83 Gbps       analog out
       │
       └── AXI ──► AXI GPIO ──► DAC_RESET, DAC_TXEN, CLKD_SYNC
```

**Data path modes** (selected per-channel via AXI DAC TPL):

| Mode | Source | Use |
|------|--------|-----|
| DDS | On-chip NCO | Tone generation (freq/scale/phase programmable) |
| SED | Fixed pattern registers | Short Transport Layer Pattern test |
| PN7/PN15 | PRBS generators | Datapath integrity verification |
| DMA | DDR via AXI DMAC | Arbitrary waveform playback |

The AD9144's receive-side XBAR remaps physical lanes {4,5,6,7} → logical {0,1,2,3}
with polarity inversion on lane 2 to match the FMC-EBZ board routing.

## 3. JESD204B Link

| Parameter | Value |
|-----------|-------|
| Mode | 4 (M=2, L=4, F=1, S=1, HD=1) |
| Subclass | 1 (deterministic latency) |
| Lane rate | 9830.4 Mbps (245.76 MHz device clock) |
| LMFC | 30.72 MHz (= fDAC / K) |
| Scrambling | Enabled |
| SYSREF edge | Rising (hardcoded in driver) |

Link startup: FPGA JESD TX enables → AD9144 sees K28.5 (CGS) → ILAS exchange →
DATA state. SYSREF aligns both LMFC counters. The `fmcdac_sysref_tune()` function
is a safety net that sweeps edge/offset if an alignment error is detected (currently
skips — rising edge works from init).

## 4. Firmware Boot Sequence

```
main()
  fmcdac_reconfig()           select clock mode + sample rate (interactive menu)
  fmcdac_setup()
    GPIO/SPI/I2C init
    AD9516 PLL lock + output programming (CLK, SYSREF, REFCLK)
    GTH transceiver init (QPLL0 lock)
    AD9144 init (DAC PLL 983 MHz, JESD config, SYSREF rising edge)
    JESD TX link enable → CGS → ILAS → DATA
    AXI DAC core init
  fmcdac_test()
    Link status poll (confirm DATA + CGS/Frame/Checksum/ILAS all 0x0F)
    STPL zero test       — DDS scale=0, verify DAC sees 0x0000
    STPL pattern test    — SED mode, verify 4 sample values per converter
    PRBS-7 + PRBS-15     — datapath integrity through full link
    SYSREF tune          — safety-net alignment sweep (skips if clean)
    SYSREF verify        — register dump confirming Subclass 1 state
    Latency readback     — dyn0/dyn1/var0/var1 for multi-boot comparison
    PHY PRBS (log-only)  — always fails (no TX-side PHY pattern source)
    DDS sweep            — 10 → 200 MHz in 5 MHz steps, 50 ms per tone
  [fmcdac_soak()]             optional 8h link stability test (ENABLE_SOAK)
  fmcdac_remove()
```

## 5. Capabilities

**Working today:**

- Full JESD204B Subclass 1 link at 983 MSPS with deterministic SYSREF alignment
- DDS tone generation with real-time frequency/scale/phase control
- Automated self-test suite: STPL (zero + pattern), PRBS-7, PRBS-15
- 10–200 MHz frequency sweep (39 points, no link drops)
- Manifest-checked builds (`gen_manifest.ps1` tracks XSA + firmware commit)
- UART-based regression verification (`verify_uart.ps1`, 13 pass/fail criteria)
- Link soak test infrastructure (8h, 1s poll, 15-min PRBS checks — gated by define)
- Multi-boot latency fingerprinting (`verify_latency.ps1`)

**Not yet validated:**

- Deterministic latency across 5+ power cycles (infrastructure built, captures pending)
- DMA waveform playback from DDR
- JESD modes other than mode 4 (500 MSPS, 600 MSPS, 2 GSPS)
- Analog output quality (SFDR, SNR — needs spectrum analyzer)
- PHY-level PRBS (needs HDL changes for GTH raw PRBS generation)

## 6. Key Files

| File | Role |
|------|------|
| `projects/fmcdac/src/app/fmcdac.c` | All firmware logic (setup, test, DDS, soak) |
| `drivers/dac/ad9144/ad9144.c` | AD9144 driver (PLL, JESD link, SYSREF) |
| `projects/fmcdac/src/app/parameters.h` | Base addresses, pin mappings, AD9516 output indices |
| `projects/fmcdac/clock_architecture.md` | Clock tree reference (frequencies, SYSREF policy) |
| `projects/fmcdac/STATUS.md` | Full status report with test results and known issues |
