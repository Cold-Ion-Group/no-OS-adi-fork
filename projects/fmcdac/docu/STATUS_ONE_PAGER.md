# FMCDAC One-Page Status

Date: 2026-04-23

## Goal

Prove the current KCU116 + AD9144-FMC-EBZ waveform-generation stack on real
hardware, freeze one baseline, and identify what the current bench can and
cannot support.

## Frozen Baseline

Baseline-of-record:

1. [`20260402T032626Z`](../capture_runs/20260402T032626Z/)

Current default platform/configuration:

1. AD9144-FMC-EBZ on KCU116
2. JESD Mode 4, Subclass 1
3. external `122.88 MHz` reference into AD9516
4. AD9144 PLL at `1966.08 MSPS`
5. `2x` interpolation
6. FPGA/JESD input rate `983.04 MSPS`
7. continuous SYSREF

What is settled:

1. DDS-band existence is real through the previously suspect `230-330 MHz`
   region; the earlier MSO22 collapse story is treated as a measurement
   artifact.
2. Steady-state SFDR from `50-400 MHz` is mechanically real and confirmed by
   [`sfdr_rerun`](../capture_runs/sfdr_rerun/), but still below the long-term
   target.
3. Throughput and UART RTT baselines exist and are stable enough to keep.

## Current Best Results

DDS-band:

1. mild droop, not disappearance
2. baseline keeps tone present and correctly placed through `330 MHz`

Steady-state SFDR:

1. about `60 dBc` at `50 MHz`
2. about `49 dBc` at `400 MHz`
3. confirmation rerun reproduced the same dominant spur family

Throughput:

1. AXI MMIO writes: `736,980 ops/s`
2. AD9144 SPI writes: `5,945 ops/s`
3. DDS pair updates: `838 ops/s`

UART RTT:

1. min `3170.6 us`
2. avg `3455.8 us`
3. max `5125.6 us`

## Closed Questions

Closed positive:

1. baseline freeze
2. DDS-band validation
3. steady-state SFDR trustworthiness as a bench baseline
4. throughput baseline
5. UART RTT baseline
6. reduced close-in phase-noise offset survey on the FSH8

Closed negative:

1. clean-from-init on the current build: not achieved
2. deterministic latency after tune on the current build: not achieved
3. strong dynamic-settling claim on the current FSH8 max-hold method: not
   achieved

## Current FSH8 Phase-Noise Status

What works:

1. marker-only offset sweep at `400 MHz`
2. keepable results:
   - about `-101.3 dBc/Hz` at `10 kHz`
   - about `-106.8 dBc/Hz` at `100 kHz`
3. confirmation rerun
   [`phase_noise_offset_400mhz_r2`](../capture_runs/phase_noise_offset_400mhz_r2/)
   stayed within about `0.5-0.7 dB`

What does not work yet:

1. raw close-in trace export on the current FSH8 `V1.58` firmware
2. compatibility probing showed `FORM*`, memory-trace commands, and both
   `TRAC:DATA? TRACE1` / `TRAC? TRACE1` are rejected or time out on this unit
3. the older trace-capture path degraded to one-point fallback in
   [`phase_noise_scout_retry_1mhz`](../capture_runs/phase_noise_scout_retry_1mhz/)
4. host automation now fails fast for trace-based requests on legacy FSH8
   firmware instead of waiting for analyzer timeouts

Meaning:

1. the FSH8 already gives a usable reduced offset baseline
2. the missing piece is either an FSH8 firmware upgrade, the exact R&S
   `V1.58` trace-export syntax, or a different export path such as
   instrument-side file transfer

## Current Dynamic SFDR Status

What is true:

1. the widened-guard host bug is fixed
2. corrected runs now exist through
   [`dynamic_sfdr_run_guard10mhz_fixed_r6`](../capture_runs/dynamic_sfdr_run_guard10mhz_fixed_r6/)
3. the corrected method is mechanically valid

Why it is not frozen:

1. repeated corrected runs still split between different dominant endpoints and
   very different margins
2. current conclusion: the asynchronous FSH8 max-hold method is still
   timing-sensitive / state-sensitive

Meaning:

1. dynamic SFDR is closed on this bench as exploratory evidence
2. it is not closed as a strong paper-grade settling claim

## Not Closable On This Bench

1. channel skew / coherence with a single-input analyzer
2. full paper-grade phase-noise curve until scripted close-in trace transfer is
   made reliable or a better instrument/mode is used

## TODOs / Blockers

1. FSH8 trace curve: ask R&S for `V1.58` trace-export syntax or upgrade the
   FSH8 firmware, then rerun `fsh_trace_probe.py`.
2. Paper-grade phase noise: replace the current marker-only offset survey with
   a dense close-in trace workflow once trace export is fixed.
3. Dynamic settling: redesign capture around a synchronized/gated measurement
   method; do not use more asynchronous max-hold reruns as closure evidence.
4. Low steady-state SFDR: root-cause the persistent spur families before
   claiming acceptance-grade spectral performance.
5. Clean init / deterministic latency: revisit SYSREF/init policy only if that
   claim remains required; the current build is closed negative.
6. Channel coherence: move to a multi-channel RF measurement setup.

## Primary Artifacts

1. [Baseline Freeze](./baseline_freeze/README.md)
2. [Current Evaluation Status](./CURRENT_EVALUATION_STATUS.md)
3. [Benchmark Results And History](./BENCHMARK_RESULTS_AND_HISTORY.md)
4. [Automation And Implementation Status](./AUTOMATION_AND_IMPLEMENTATION_STATUS.md)
