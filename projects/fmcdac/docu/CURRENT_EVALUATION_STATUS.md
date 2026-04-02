# FMCDAC Current Evaluation Status

Date: 2026-04-02

## Purpose

This is the top-level status note for the current FMCDAC evaluation effort.
It summarizes:

1. what we are trying to prove
2. what we changed in firmware and host tooling
3. what the latest measurements actually show
4. what remains open

The detail has been split into supporting notes so this file stays readable:

1. [Benchmark Results And History](./BENCHMARK_RESULTS_AND_HISTORY.md)
2. [Automation And Implementation Status](./AUTOMATION_AND_IMPLEMENTATION_STATUS.md)
3. [Prior Explorations And Architectural Options](./PRIOR_EXPLORATIONS_AND_ARCH_OPTIONS.md)
4. [FSH8 DDS Benchmark Automation](./NCO_SCOPE_AUTOMATION.md)

## Ultimate Goals

Current goals for this phase:

1. Validate full DDS output behavior with RF measurements on real hardware.
2. Refute or confirm the earlier apparent amplitude collapse around
   `260-290 MHz`.
3. Establish a steady-state SFDR baseline from `50 MHz` to `400 MHz`.
4. Establish firmware-side throughput baselines:
   - AXI MMIO write rate
   - AD9144 SPI write rate
   - DDS pair retune rate
5. Establish a host-visible UART latency baseline.
6. Keep the full bring-up and benchmarking flow reproducible from:
   - [fmcdac.c](../src/app/fmcdac.c)
   - [ad9144.c](../../../drivers/dac/ad9144/ad9144.c)
   - [run_nco_scope_test.py](../run_nco_scope_test.py)

Still outside the current automation scope:

1. acceptance-grade phase-noise measurement
2. dynamic SFDR during rapid steps or chirps
3. HDL root-cause experiments unless DDS-band or SFDR results push us back
   toward a digital explanation

## Current Bottom Line

The main conclusion has changed since the early scope-based investigation.

1. The severe amplitude collapse seen on the `200 MHz` MSO22 is no longer the
   leading explanation.
2. The R&S FSH8 measurements show only mild DDS-band droop through
   `230-330 MHz`, not a disappearance of the tone.
3. The earlier scope result is now best treated as a measurement artifact,
   likely driven by instrument bandwidth and/or measurement-path limitations.
4. The current open issue is no longer DDS-band amplitude collapse. It is now
   spectral purity:
   - the SFDR automation is working mechanically
   - the measured SFDR baseline is currently far below the long-term target
5. NCO is no longer a gating diagnostic. The evaluation focus is now full DDS.

## Latest Primary Baseline

Latest full run:

1. [summary.json](../capture_runs/20260402T032626Z/summary.json)
2. [sfdr_results.csv](../capture_runs/20260402T032626Z/sfdr_results.csv)
3. [throughput.json](../capture_runs/20260402T032626Z/throughput.json)
4. [uart_rtt.json](../capture_runs/20260402T032626Z/uart_rtt.json)
5. [uart.log](../capture_runs/20260402T032626Z/uart.log)

Headline results from that run:

### DDS-band

Relative to the `10 MHz` reference:

| Frequency | Power Delta |
|-----------|-------------|
| 200 MHz | -0.587 dB |
| 230 MHz | -0.818 dB |
| 260 MHz | -1.621 dB |
| 290 MHz | -1.244 dB |
| 330 MHz | -2.768 dB |

Interpretation:

1. DDS output remains present and correctly placed through the previously
   suspected failure region.
2. The analyzer result does not support the earlier "gone above ~290 MHz"
   conclusion.

### SFDR

Current steady-state SFDR baseline:

| Carrier | SFDR |
|---------|------|
| 50 MHz | 59.96 dBc |
| 100 MHz | 58.05 dBc |
| 150 MHz | 55.37 dBc |
| 200 MHz | 57.44 dBc |
| 250 MHz | 55.19 dBc |
| 300 MHz | 53.87 dBc |
| 350 MHz | 52.39 dBc |
| 400 MHz | 48.59 dBc |

Interpretation:

1. The SFDR automation is now producing nonzero, structured results.
2. These values are still well below the long-term `>= 85-90 dBc` goal.
3. The next evaluation focus should therefore be spectral-purity validation,
   not DDS-band existence.

### Throughput

From the latest firmware benchmark:

| Test | Result |
|------|--------|
| `axi_mmio_write` | `736,980 ops/s` |
| `ad9144_spi_write` | `5,945 ops/s` |
| `dds_pair_update` | `838 ops/s` |

### UART RTT

From the latest host benchmark:

| Metric | Value |
|--------|-------|
| Samples | `16` |
| Min RTT | `3170.6 us` |
| Avg RTT | `3455.8 us` |
| Max RTT | `5125.6 us` |
| Avg one-way estimate | `1727.9 us` |

## What Is Implemented

Implemented in firmware:

1. fixed startup defaults for rate/clock configuration
2. paused `DDS-BAND` diagnostics
3. paused `SFDR-TEST` diagnostics
4. firmware-side throughput benchmark
5. UART ping/pong RTT service

Implemented in host tooling:

1. direct `make run` orchestration from `projects/fmcdac`
2. UART prompt handling for DDS-band, SFDR, throughput, and RTT
3. FSH8 DDS-band measurement
4. FSH8 SFDR measurement using segmented spur sweeps
5. artifact generation:
   - `summary.json`
   - per-step CSV/JSON
   - `sfdr_results.csv`
   - `throughput.json`
   - `uart_rtt.json`

Earlier explorations that are now tracked separately:

1. 500 MHz path and higher-rate architecture options
2. batched SYNC and shadow-cache work
3. DMA and waveform-memory path context
4. CORDIC-development context
5. EXT_SYNC and related architectural cleanup items

## Current Open Questions

1. Are the current SFDR results fundamentally real, or are some detected worst
   spurs still influenced by analyzer setup choices?
2. How much of the observed SFDR trend is converter/board behavior versus bench
   configuration?
3. Should the NCO diagnostic remain in the firmware now that DDS-band is the
   primary focus?

## Recommended Next Steps

1. Treat [20260402T032626Z](../capture_runs/20260402T032626Z/) as the current
   primary baseline.
2. Refine SFDR measurement confidence before drawing hard architectural
   conclusions.
3. Update longer-lived project status docs only after the DDS-band/SFDR story is
   stable.
4. Keep phase noise and dynamic SFDR explicitly marked as pending.
