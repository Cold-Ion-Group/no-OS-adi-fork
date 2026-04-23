# JESD Mode 9: FPGA-Generated Signals Above 491 MHz

> Historical note: this file is retained as architectural design history, not
> current execution guidance. Current benchmarking and closure status live in
> [STATUS_ONE_PAGER.md](./STATUS_ONE_PAGER.md) and
> [CURRENT_EVALUATION_STATUS.md](./CURRENT_EVALUATION_STATUS.md).

**Date**: 2026-04-02 (updated)
**Status**: Architectural option — not currently in active development
**Platform**: AD9144-FMC-EBZ on KCU116 (Kintex UltraScale XCKU5P)

---

## Background

The target output band is **50–500 MHz**. The current architecture (JESD Mode 4,
M=2 L=4 F=1 S=1) runs the FPGA DDS at 983.04 MSPS, giving a Nyquist limit of
491.52 MHz. The last ~8.5 MHz of the target band falls outside the first Nyquist
zone.

With 2x interpolation (now the default, DAC PLL = 1966.08 MHz), the AD9144 DAC
runs at 1966.08 MSPS internally, but the **FPGA still only sends 983 M samples/s**
over the JESD link. Interpolation improves signal quality within the 0–491 MHz
band but does not extend the FPGA's addressable bandwidth.

To generate **arbitrary, FPGA-synthesized signals above 491 MHz**, the FPGA input
rate to the JESD link must increase. This requires a different JESD mode.

---

## Evaluation Status (April 2026)

The earlier 230–330 MHz amplitude collapse observed on the Tek MSO22 (200 MHz
bandwidth) has been **refuted** by R&S FSH8 spectrum analyzer measurements:

- DDS-band droop through 230–330 MHz is mild (< 3 dB), not catastrophic
- The tone remains present and correctly placed at all tested frequencies
- The earlier scope result is now treated as a measurement artifact driven by
  instrument bandwidth limitations

See [CURRENT_EVALUATION_STATUS.md](./CURRENT_EVALUATION_STATUS.md) and
[BENCHMARK_RESULTS_AND_HISTORY.md](./BENCHMARK_RESULTS_AND_HISTORY.md) for the
latest FSH8 baselines.

**Consequence for Mode 9**: The amplitude collapse is no longer a justification
for pursuing Mode 9. Mode 9 remains valid only for the original purpose: FPGA-
generated content above the first Nyquist zone (491.52 MHz).

---

## AD9144 Mode 9 Parameters

> [!IMPORTANT]
> The AD9144 datasheet defines mode numbers differently for single-link and
> dual-link configurations. Mode numbering is context-dependent. The local
> driver (`drivers/dac/ad9144/ad9144.c`) defines the mode table that the
> firmware actually programs. That is the ground truth for this project.

### Driver mode table (ground truth)

From `ad9144.c` line 58:

```c
static const struct ad9144_jesd204_link_mode ad9144_jesd204_link_modes[] = {
    /* ID, M, L, S, F */
    {  0, 4, 8, 1, 1 },
    {  1, 4, 8, 2, 2 },
    {  2, 4, 4, 1, 2 },
    {  3, 4, 2, 1, 4 },
    {  4, 2, 4, 1, 1 },   ← current default
    {  5, 2, 4, 2, 2 },
    {  6, 2, 2, 1, 2 },
    {  7, 1, 1, 1, 4 },
    {  9, 1, 2, 1, 1 },   ← "mode 9"
    { 10, 1, 1, 1, 2 },
};
```

### Mode 9 as defined in this driver

| Parameter | Value |
|-----------|-------|
| M (converters) | 1 |
| L (lanes) | 2 |
| S (samples/conv/frame) | 1 |
| F (octets/frame/lane) | 1 |

This is a **single-converter, 2-lane** configuration. It does **not** double
the FPGA input rate at the same lane rate — it halves the lane count and uses
one converter. At the same lane rate (9830.4 Mbps):

- Input sample rate = lane_rate × L / (M × S × N' × 10/8)
- = 9830.4 × 2 / (1 × 1 × 16 × 1.25) = 983.04 MSPS

**Mode 9 in this driver does NOT increase the FPGA input rate.** It provides
single-converter operation with 2 lanes, not higher bandwidth.

### Comparison with earlier doc claims

| Source | M | L | S | F | Implication |
|--------|---|---|---|---|-------------|
| This driver (ground truth) | 1 | 2 | 1 | 1 | 1 converter, 2 lanes, same rate |
| Earlier MODE9 doc (wrong) | 2 | 8 | 1 | 1 | 2 converters, 8 lanes, 2× rate |
| Earlier dds_tuning doc (wrong) | 1 | 4 | 2 | 1 | 1 converter, 4 lanes, 2× rate |
| AD9144 datasheet (context-dependent) | varies | varies | — | — | Mode numbering differs between single-link and dual-link tables |

> [!WARNING]
> The earlier versions of this document and `dds_tuning_status.md` both had
> incorrect mode-9 parameters that did not match the driver. Those documents
> have been removed or corrected.

---

## Current vs Mode 9 Architecture

| Parameter | Mode 4 (current) | Mode 9 (this driver) |
|-----------|-------------------|----------------------|
| Converters (M) | 2 | 1 |
| Lanes (L) | 4 | 2 |
| Octets/frame (F) | 1 | 1 |
| Samples/conv/frame (S) | 1 | 1 |
| Lane rate | 9830.4 Mbps | 9830.4 Mbps (unchanged) |
| FPGA input rate | 983.04 MSPS | 983.04 MSPS (unchanged) |
| DAC output rate (with 2x interp) | 1966.08 MSPS | 983.04 MSPS (1 conv only) |
| FPGA DDS Nyquist | 491.52 MHz | 491.52 MHz (unchanged) |

**Key result**: Mode 9 in this driver does not solve the >491 MHz bandwidth
problem. To actually double the FPGA input rate, a different approach is needed.

---

## Paths to >491 MHz FPGA Bandwidth

### Option 1: Mode 0 (M=4, L=8, F=1)

This mode doubles the aggregate lane bandwidth by using 8 lanes:

- Lane rate: 9830.4 Mbps × 8 lanes = 78.6 Gbps aggregate
- FPGA input rate: 1966.08 MSPS per converter (4 converters)
- **Requires all 8 FMC HPC differential pairs routed to GT pins**
- **Blocking prerequisite**: verify KCU116 and FMC-EBZ both route DP4–DP7

### Option 2: AD9144 on-chip NCO (firmware-only, no HDL)

- Keep Mode 4 (L=4) with 2x interpolation (current default)
- FPGA DDS covers 0–491 MHz
- AD9144 on-chip NCO (`fcenter_shift` / `ad9144_set_nco()`) places a carrier
  above 491 MHz; FPGA DDS provides baseband modulation around that center
- **Limitation**: modulation bandwidth is limited to the DDS baseband range
- **Already accessible** through `ad9144_set_nco()` in the driver

### Option 3: 2nd Nyquist zone (image mode)

- Program DDS to fDAC − ftarget, use external bandpass filter to select image
- Enable AD9144 inverse-sinc filter (`INVSINC_EN`) for flatness
- **Requires external RF filter hardware**

### Recommendation

| Target | Best approach | Effort | Risk |
|--------|--------------|--------|------|
| ≤ 491 MHz | Current Mode 4 + 2x interp | None (working) | None |
| 491–983 MHz, CW tones | Option 2 (on-chip NCO) | Low (FW only) | Low |
| 491–983 MHz, arbitrary waveforms | Option 1 (8-lane mode) | High (HDL+FW) | High |
| >491 MHz, image-zone | Option 3 (BPF + image) | Medium (RF HW) | Medium |

---

## HDL Changes (if 8-lane mode is pursued)

These are the same as previously documented but apply to Mode 0 (L=8), not
the incorrectly-labeled "mode 9":

1. **GTH expansion**: 4 → 8 TX lanes (DP0–DP7)
2. **AXI JESD204 TX**: `NUM_LANES` = 8
3. **AXI DAC TPL**: DPW must increase (8 samples/clock at 245.76 MHz)
4. **Constraints**: pin LOC + timing for DP4–DP7
5. **XBAR**: 8-lane physical→logical mapping

### Blocking prerequisites (unchanged)

| # | Check | Impact if fails |
|---|-------|-----------------|
| A-1 | KCU116 FMC HPC routes DP4–DP7 to GT pins | Mode 0 impossible on this board |
| A-2 | AD9144-FMC-EBZ wires all 8 SERDES pairs to FMC | Mode 0 impossible on this mezzanine |
| A-3 | ADI AXI DAC TPL supports DPW=8 | IP modification needed |
| A-4 | Timing closes at 245.76 MHz with DPW=8 | Pipeline or clock changes needed |

---

## References

- AD9144 datasheet Rev. E (JESD mode tables — note single-link vs dual-link numbering)
- Local driver: `drivers/dac/ad9144/ad9144.c` lines 50–70 (mode table)
- AD9144-FMC-EBZ schematic (SERDES lane routing)
- KCU116 User Guide UG1237, Table 1-22 (FMC HPC GT lanes)
- XCKU5P datasheet DS892 (GTH bank availability)
- Current working config: Mode 4, 4 lanes, 2x interpolation, DAC PLL 1966.08 MHz
