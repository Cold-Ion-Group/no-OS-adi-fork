# Automation And Implementation Status

Date: 2026-04-02

This note captures the current implementation state of the firmware diagnostics
and the host automation used to benchmark the FMCDAC platform.

## Main Code Files

Primary firmware files:

1. [fmcdac.c](../src/app/fmcdac.c)
2. [ad9144.c](../../../drivers/dac/ad9144/ad9144.c)

Primary host tools:

1. [run_nco_scope_test.py](../run_nco_scope_test.py)
2. [dds_band_plot.py](../dds_band_plot.py)
3. [plot_dds_band_summary.py](../plot_dds_band_summary.py)

Primary docs:

1. [CURRENT_EVALUATION_STATUS.md](./CURRENT_EVALUATION_STATUS.md)
2. [BENCHMARK_RESULTS_AND_HISTORY.md](./BENCHMARK_RESULTS_AND_HISTORY.md)
3. [PRIOR_EXPLORATIONS_AND_ARCH_OPTIONS.md](./PRIOR_EXPLORATIONS_AND_ARCH_OPTIONS.md)
4. [NCO_SCOPE_AUTOMATION.md](./NCO_SCOPE_AUTOMATION.md)

## Firmware Status

### Current diagnostic features in `fmcdac.c`

Implemented:

1. fixed startup defaults for rate and clock selection
2. `DDS-BAND` paused diagnostic:
   - `10 MHz`
   - `100 MHz`
   - `200 MHz`
   - `230-330 MHz` in `10 MHz` steps
3. `SFDR-TEST` paused diagnostic:
   - `50-400 MHz` in `50 MHz` steps
4. `THROUGHPUT` benchmark:
   - raw AXI MMIO writes
   - AD9144 SPI writes
   - DDS pair retunes
5. `UART-RTT` ping/pong service for host-side timing

Still present but no longer primary:

1. `NCO-TEST`

### Relevant earlier driver changes in `ad9144.c`

Implemented earlier in support of the evaluation:

1. corrected AD9144 NCO sample-rate handling
2. made `carrier = 0` explicitly disable NCO
3. fixed legacy init state so PLL/rate context is valid

Related earlier system explorations now tracked separately:

1. 500 MHz output paths and higher-rate architecture options
2. batched SYNC and shadow cache
3. DMA / arbitrary-waveform path
4. CORDIC-development context
5. EXT_SYNC cleanup work

Current role of those changes:

1. they remain correct and useful
2. they are not the current primary focus because the evaluation is now centered
   on full DDS behavior and SFDR

## Host Automation Status

### Current role of `run_nco_scope_test.py`

Implemented:

1. optional `make run` launch from `projects/fmcdac`
2. optional Xilinx environment setup via `--xilinx-settings`
3. UART coordination for paused firmware prompts
4. FSH8 measurement flow for:
   - DDS-band
   - SFDR
   - throughput collection
   - UART RTT collection
5. artifact generation into `capture_runs/<timestamp>/`

### Current workflow policy

Primary path:

1. DDS-band
2. SFDR
3. throughput
4. UART RTT

Secondary path:

1. NCO is opt-in only via `--run-nco-test`

### Current measurement strategy

DDS-band:

1. narrow analyzer span
2. marker-based peak capture
3. optional raw trace via `--capture-trace`

SFDR:

1. narrow carrier sweep
2. left-side spur sweep
3. right-side spur sweep
4. guard band around the carrier
5. worst remaining spur selected from the left/right sweep results

Reason for the current SFDR strategy:

1. simple marker-window logic could reacquire the carrier as the spur
2. forced wideband `TRAC:DATA? TRACE1` capture timed out on the FSH8
3. segmented marker sweeps are the current compromise that works on this bench

## Artifact Status

### Produced by the current flow

Per run:

1. `summary.json`
2. `uart.log`
3. `dds_band_plot.csv`
4. `dds_band_plot.svg`
5. `sfdr_results.csv`
6. `throughput.json`
7. `uart_rtt.json`

Per step:

1. `stepNN_<name>.csv`
2. `stepNN_<name>.json`

### Current meaning of the key artifacts

`summary.json`

1. full run summary
2. contains analyzer settings
3. contains per-step metrics
4. contains parsed throughput and UART RTT sections

`sfdr_results.csv`

1. reduced SFDR summary by carrier frequency
2. should now be treated as the first place to compare SFDR runs

`throughput.json`

1. machine-readable baseline of firmware software update rates

`uart_rtt.json`

1. machine-readable baseline of host-to-firmware UART round-trip latency

## Bench Setup Status

Current preferred instrument:

1. R&S FSH8

De-emphasized instrument for this question:

1. Tek MSO22 with `200 MHz` bandwidth

Current preferred physical setup:

1. direct coax
2. `50 ohm` input
3. known attenuation
4. conservative analyzer reference level

Current suggested analyzer defaults:

1. `RBW = 100 kHz`
2. `VBW = 100 kHz`
3. `sweep_count = 3`
4. `trace_mode = average`
5. `detector = positive`

## Current Gaps

Not yet closed:

1. acceptance-quality SFDR validation
2. phase-noise measurement near `~400 MHz`
3. dynamic SFDR during rapid steps or chirps
4. optional HDL experiments if future evidence points back toward a digital
   issue

## Practical Next Steps

1. keep [20260402T032626Z](../capture_runs/20260402T032626Z/) as the current
   reference run
2. use the current flow to repeat SFDR with carefully controlled attenuation and
   analyzer configuration
3. only revisit NCO or HDL-focused experiments if DDS-band or SFDR evidence
   stops making sense
