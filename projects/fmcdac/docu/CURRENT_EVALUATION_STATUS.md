# FMCDAC Current Evaluation Status

Date: 2026-04-05

## Purpose

This is the top-level status note for the current FMCDAC evaluation effort.
It summarizes:

1. what we are trying to prove
2. what we changed in firmware and host tooling
3. what the latest measurements actually show
4. what remains open

The detail has been split into supporting notes so this file stays readable:

1. [One-Page Status](./STATUS_ONE_PAGER.md)
2. [Baseline Freeze](./baseline_freeze/README.md)
3. [Benchmark Results And History](./BENCHMARK_RESULTS_AND_HISTORY.md)
4. [Automation And Implementation Status](./AUTOMATION_AND_IMPLEMENTATION_STATUS.md)
5. [Prior Explorations And Architectural Options](./PRIOR_EXPLORATIONS_AND_ARCH_OPTIONS.md)
6. [FSH8 DDS Benchmark Automation](./NCO_SCOPE_AUTOMATION.md)

## Ultimate Goals

Current goals for this phase:

1. Validate full DDS output behavior with RF measurements on real hardware.
2. Freeze the DDS-band conclusion so the project stops re-litigating amplitude
   existence around `260-290 MHz`.
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
2. channel skew / coherence capture on a multi-channel bench
3. HDL root-cause experiments unless DDS-band or SFDR results push us back
   toward a digital explanation

## Priority Action Status

1. Freeze one canonical current baseline: **done**
   - [`20260402T032626Z`](../capture_runs/20260402T032626Z/) is the
     baseline-of-record
   - the frozen summary now lives in [Baseline Freeze](./baseline_freeze/README.md)
2. Make SFDR trustworthy before drawing architecture conclusions: **done for the
   steady-state bench baseline**
   - [`sfdr_rerun`](../capture_runs/sfdr_rerun/) reproduced the same dominant
     spur family with fixed analyzer settings
   - this closes the "is the current steady-state SFDR baseline mechanically
     real?" question
   - it does **not** close SFDR root-cause attribution or acceptance-grade SFDR
3. Close the remaining paper-grade blockers: **not done**
   - clean recovery after tune is confirmed, but post-tune deterministic
     latency is not
   - the old raw-trace phase-noise path is blocked by the FSH8
     `TRAC:DATA? TRACE1` timeout, but the new marker-only offset-sweep method
     has now produced confirmed preliminary `400 MHz` offset results on the
     same bench
   - clean-from-init SYSREF/latency is now measurable directly, and the current
     build does not pass that stronger claim
   - dynamic SFDR is now mechanically implemented, the widened-guard host bug
     is fixed, and repeated corrected `10 MHz`-guard runs are now archived
   - those corrected dynamic reruns still do not converge, so the current
     asynchronous FSH8 max-hold method is now closed as non-repeatable rather
     than left pending for more blind reruns
   - channel skew/coherence is deferred until a multi-channel RF instrument is
     available

## Current Bottom Line

The main conclusion has changed since the early scope-based investigation.

1. The severe amplitude collapse seen on the `200 MHz` MSO22 is no longer the
   leading explanation.
2. The R&S FSH8 measurements show only mild DDS-band droop through
   `230-330 MHz`, not a disappearance of the tone.
3. The earlier scope result is now best treated as a measurement artifact,
   likely driven by instrument bandwidth and/or measurement-path limitations.
4. The current open issue is no longer DDS-band amplitude collapse. It is now
   spectral quality:
   - the steady-state SFDR flow is now confirmed as a usable bench baseline
   - the measured SFDR baseline is still far below the long-term target
5. The replacement clean-init rerun is now a valid 5/5 negative result for the
   stronger SYSREF/latency claim:
   - all 5 cycles came up pre-tune with `SYSREF_STATUS = 0x00000003`
   - all 5 cycles required tune to reach `0x00000001`
   - post-tune latency still split between `0x03/0x03/0x0A/0x0A` and
     `0x02/0x02/0x0A/0x0A`
   - the accurate current statement is therefore "recovers cleanly after tune,"
     not "deterministic after tune"
6. The marker-only phase-noise offset method now works on the current FSH8
   bench at `400 MHz`:
   - `10 kHz` offset: about `-101.3 dBc/Hz`
   - `100 kHz` offset: about `-106.8 dBc/Hz`
   - the `1 kHz` point is not credible enough to keep as a claim
   - the confirmation rerun stayed within about `0.5-0.7 dB` at `10 kHz` and
     `100 kHz`, which is good enough to freeze the reduced offset survey as the
     current FSH8 close-in baseline
7. Dynamic retune-burst interpretation is now on firmer ground:
   - [`dynamic_sfdr_run_guard10mhz`](../capture_runs/dynamic_sfdr_run_guard10mhz/)
     is now treated as a bug-finding run because the host widened the intended
     windows but still excluded only a `2 MHz` spur guard
   - [`dynamic_sfdr_run_guard10mhz_fixed`](../capture_runs/dynamic_sfdr_run_guard10mhz_fixed/)
     is the first valid widened-guard result
   - later corrected reruns in
     [`dynamic_sfdr_run_guard10mhz_fixed_r2`](../capture_runs/dynamic_sfdr_run_guard10mhz_fixed_r2/),
     [`dynamic_sfdr_run_guard10mhz_fixed_r3`](../capture_runs/dynamic_sfdr_run_guard10mhz_fixed_r3/),
     [`dynamic_sfdr_run_guard10mhz_fixed_r4`](../capture_runs/dynamic_sfdr_run_guard10mhz_fixed_r4/),
     [`dynamic_sfdr_run_guard10mhz_fixed_r5`](../capture_runs/dynamic_sfdr_run_guard10mhz_fixed_r5/),
     and
     [`dynamic_sfdr_run_guard10mhz_fixed_r6`](../capture_runs/dynamic_sfdr_run_guard10mhz_fixed_r6/)
     still split between different dominant endpoints and very different
     guarded margins
   - treat the current dynamic method as mechanically valid but still
     timing-sensitive / state-sensitive on the current bench
   - dynamic SFDR is therefore closed for this bench with a non-repeatable
     result, not frozen as a strong settling baseline
8. NCO is no longer a gating diagnostic. The evaluation focus is now full DDS
   plus the remaining paper-grade blockers.

## Baseline Of Record

The current baseline-of-record is frozen here:

1. [Baseline Freeze](./baseline_freeze/README.md)
2. [summary.json](../capture_runs/20260402T032626Z/summary.json)
3. [sfdr_results.csv](../capture_runs/20260402T032626Z/sfdr_results.csv)
4. [throughput.json](../capture_runs/20260402T032626Z/throughput.json)
5. [uart_rtt.json](../capture_runs/20260402T032626Z/uart_rtt.json)
6. [uart.log](../capture_runs/20260402T032626Z/uart.log)

Later confirmation runs that support, but do not replace, the baseline:

1. [sfdr_rerun](../capture_runs/sfdr_rerun/)
2. [boot_repeatability_20260404T165206Z](../capture_runs/boot_repeatability_20260404T165206Z/)
3. [phase_noise_offset_400mhz](../capture_runs/phase_noise_offset_400mhz/)
4. [dynamic_sfdr_run](../capture_runs/dynamic_sfdr_run/)
5. [dynamic_sfdr_run_r2](../capture_runs/dynamic_sfdr_run_r2/)
6. [dynamic_sfdr_run_r3](../capture_runs/dynamic_sfdr_run_r3/)
7. [dynamic_sfdr_run_r4](../capture_runs/dynamic_sfdr_run_r4/)
8. [dynamic_sfdr_run_guard10mhz](../capture_runs/dynamic_sfdr_run_guard10mhz/)
9. [dynamic_sfdr_run_guard10mhz_fixed](../capture_runs/dynamic_sfdr_run_guard10mhz_fixed/)
10. [boot_repeatability_clean_init](../capture_runs/boot_repeatability_clean_init/)
11. [boot_repeatability_clean_init_rerun](../capture_runs/boot_repeatability_clean_init_rerun/)
12. [phase_noise_offset_400mhz_r2](../capture_runs/phase_noise_offset_400mhz_r2/)
13. [dynamic_sfdr_run_guard10mhz_fixed_r2](../capture_runs/dynamic_sfdr_run_guard10mhz_fixed_r2/)
14. [dynamic_sfdr_run_guard10mhz_fixed_r3](../capture_runs/dynamic_sfdr_run_guard10mhz_fixed_r3/)
15. [dynamic_sfdr_run_guard10mhz_fixed_r4](../capture_runs/dynamic_sfdr_run_guard10mhz_fixed_r4/)
16. [dynamic_sfdr_run_guard10mhz_fixed_r5](../capture_runs/dynamic_sfdr_run_guard10mhz_fixed_r5/)
17. [dynamic_sfdr_run_guard10mhz_fixed_r6](../capture_runs/dynamic_sfdr_run_guard10mhz_fixed_r6/)

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
3. DDS-band existence should now be treated as settled for the current default
   platform and not reopened unless the hardware path or default clocking mode
   changes.

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
2. A later confirmation run in [`sfdr_rerun`](../capture_runs/sfdr_rerun/)
   reproduced the same dominant spur family at all eight carriers using the
   same analyzer settings.
3. The confirmation run stayed within about `+0.06` to `+0.59 dB` of the
   original `20260402T032626Z` SFDR values, which is good enough to treat this
   as a trusted steady-state bench baseline.
4. These values are still well below the long-term `>= 85-90 dBc` goal.
5. The next evaluation focus is therefore the remaining paper-grade
   measurements, not DDS-band revalidation.

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
4. paused `DYNAMIC-SFDR` retune-burst diagnostic
5. firmware-side throughput benchmark
6. UART ping/pong RTT service

Implemented in host tooling:

1. direct `make run` orchestration from `projects/fmcdac`
2. UART prompt handling for DDS-band, SFDR, dynamic retune, throughput, and RTT
3. FSH8 DDS-band measurement
4. FSH8 SFDR measurement using segmented spur sweeps
5. close-in carrier trace capture during selected SFDR steps
6. dynamic retune burst capture with intended-tone windows plus guarded spur search
7. boot-repeatability capture via
   [`capture_boot_repeatability.py`](../capture_boot_repeatability.py)
8. artifact generation:
   - `summary.json`
   - per-step CSV/JSON
   - `sfdr_results.csv`
   - `phase_noise_results.csv`
   - `dynamic_sfdr_results.csv`
   - `throughput.json`
   - `uart_rtt.json`
   - `boot_repeatability.json`
   - `boot_repeatability.csv`

Earlier explorations that are now tracked separately:

1. 500 MHz path and higher-rate architecture options
2. batched SYNC and shadow-cache work
3. DMA and waveform-memory path context
4. CORDIC-development context
5. EXT_SYNC and related architectural cleanup items

## Current Open Questions

1. What is driving the still-low but now repeatable steady-state SFDR baseline,
   especially the persistent `50 MHz -> 729 MHz` spur?
2. Is the current "stable after tune" SYSREF behavior acceptable as the working
   baseline, or should the init/tune policy be cleaned up further?
3. Is the current marker-only offset sweep strong enough for the intended
   paper-grade phase-noise claim, or is a different instrument still required
   for a fuller close-in curve?
4. Can the current SYSREF/init policy ever produce clean-from-init operation,
   or should both the no-tune and deterministic-after-tune latency claims be
   dropped for this build?
5. What method replaces the asynchronous FSH8 max-hold dynamic capture if a
   strong retune-settling claim is still required?
6. Can the current FSH8 `V1.58` firmware support dense trace export through a
   different SCPI or file-transfer path, or is a firmware upgrade required?

## Recommended Next Steps

1. Keep [Baseline Freeze](./baseline_freeze/README.md) and
   [20260402T032626Z](../capture_runs/20260402T032626Z/) as the current
   baseline-of-record.
2. Use [sfdr_rerun](../capture_runs/sfdr_rerun/) only as a confirmation run,
   not as a baseline replacement.
3. Keep
   [phase_noise_offset_400mhz](../capture_runs/phase_noise_offset_400mhz/)
   and
   [phase_noise_offset_400mhz_r2](../capture_runs/phase_noise_offset_400mhz_r2/)
   as the confirmed reduced close-in offset baseline on the FSH8, and keep
   [phase_noise_scout_retry_1mhz](../capture_runs/phase_noise_scout_retry_1mhz/)
   as evidence that the raw-trace export path is still blocked.
4. Keep
   [boot_repeatability_clean_init_rerun](../capture_runs/boot_repeatability_clean_init_rerun/)
   as the current direct clean-init evidence set. It closes the question as a
   negative result on this build: tune is always required, and latency is not
   deterministic across the 5-cycle rerun.
5. Keep [dynamic_sfdr_run](../capture_runs/dynamic_sfdr_run/),
   [dynamic_sfdr_run_r2](../capture_runs/dynamic_sfdr_run_r2/),
   [dynamic_sfdr_run_r3](../capture_runs/dynamic_sfdr_run_r3/), and
   [dynamic_sfdr_run_r4](../capture_runs/dynamic_sfdr_run_r4/) as historical
   evidence that the original `2 MHz` dynamic method was unstable. Keep
   [dynamic_sfdr_run_guard10mhz](../capture_runs/dynamic_sfdr_run_guard10mhz/)
   as the bug-finding run that exposed the overlap problem, and keep
   [dynamic_sfdr_run_guard10mhz_fixed](../capture_runs/dynamic_sfdr_run_guard10mhz_fixed/)
   through
   [dynamic_sfdr_run_guard10mhz_fixed_r6](../capture_runs/dynamic_sfdr_run_guard10mhz_fixed_r6/)
   as the corrected widened-guard evidence set.
6. Treat the current widened-guard dynamic method as closed on this bench with
   a non-repeatable result. Do not spend more time on blind reruns unless the
   capture method changes materially.
7. Defer channel skew / coherence evidence until a multi-channel RF instrument
   is available; the single-input FSH8 cannot close that claim.
8. Only revisit deeper architecture or HDL root-cause work if those results
   point back toward clocking or digital transport.

## TODOs / Blockers

1. Full phase-noise curve:
   - current blocker is FSH8 `V1.58` trace-export compatibility
   - `--phase-noise-span-hz` and `--capture-trace` now fail fast on legacy
     firmware
   - next action is R&S `V1.58` syntax / firmware upgrade / file-export probe
2. Dynamic settling:
   - corrected widened-guard evidence is non-repeatable on asynchronous
     max-hold capture
   - next action is a synchronized or gated measurement method
3. SFDR improvement:
   - steady-state baseline is repeatable but below target
   - next action is spur-family root cause, not more baseline reruns
4. Clean init / deterministic latency:
   - current build is measured and closed negative
   - next action requires SYSREF/init-policy changes if the claim matters
5. Channel coherence:
   - blocked by single-input analyzer
   - next action requires a multi-channel RF bench
