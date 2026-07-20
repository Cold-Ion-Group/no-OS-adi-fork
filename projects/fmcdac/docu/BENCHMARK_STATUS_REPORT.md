# FMCDAC Benchmark Status Report

Date: 2026-07-16

This report is a consolidated view of the benchmarking work in `projects/fmcdac`.
It is based first on raw result artifacts and only then on the supporting docs
used for context. The goal is to separate measured evidence from the narrative
around it.

Raw evidence reviewed first:

1. [20260402T032626Z summary.json](../capture_runs/20260402T032626Z/summary.json)
2. [20260402T032626Z sfdr_results.csv](../capture_runs/20260402T032626Z/sfdr_results.csv)
3. [20260402T032626Z throughput.json](../capture_runs/20260402T032626Z/throughput.json)
4. [20260402T032626Z uart_rtt.json](../capture_runs/20260402T032626Z/uart_rtt.json)
5. [sfdr_rerun sfdr_results.csv](../capture_runs/sfdr_rerun/sfdr_results.csv)
6. [boot_repeatability_clean_init_rerun boot_repeatability.json](../capture_runs/boot_repeatability_clean_init_rerun/boot_repeatability.json)
7. [phase_noise_offset_400mhz_r2 phase_noise_offset_results.csv](../capture_runs/phase_noise_offset_400mhz_r2/phase_noise_offset_results.csv)
8. [phase_noise_trace_binary_r4 summary.json](../capture_runs/phase_noise_trace_binary_r4/summary.json)
9. [fsh_trace_debug fsh_trace_probe_200mhz.json](../capture_runs/fsh_trace_debug/fsh_trace_probe_200mhz.json)
10. [dynamic_sfdr_run_guard10mhz_fixed_r6 dynamic_sfdr_results.csv](../capture_runs/dynamic_sfdr_run_guard10mhz_fixed_r6/dynamic_sfdr_results.csv)
11. [fsh_scheduler_preload_perstep_200_210_rf_enforced scheduler_benchmark_suite.json](../capture_runs/fsh_scheduler_preload_perstep_200_210_rf_enforced/scheduler_benchmark_suite.json)
12. [fsh_scheduler_stream_perstep_200_210_rf_enforced scheduler_benchmark_suite.json](../capture_runs/fsh_scheduler_stream_perstep_200_210_rf_enforced/scheduler_benchmark_suite.json)
13. [stream_bringup scheduler_benchmark_suite.json](../capture_runs/stream_bringup/scheduler_benchmark_suite.json)

Supporting context docs:

Supporting references:

1. [Current Evaluation Status](./CURRENT_EVALUATION_STATUS.md)
2. [Benchmark Results And History](./BENCHMARK_RESULTS_AND_HISTORY.md)
3. [Automation And Implementation Status](./AUTOMATION_AND_IMPLEMENTATION_STATUS.md)
4. [Scheduler Benchmark Suite](./SCHEDULER_BENCHMARK_SUITE.md)
5. [Scheduler Handoff Status](./SCHEDULER_HANDOFF_STATUS.md)
6. [Baseline Freeze](./baseline_freeze/README.md)

## Executive Summary

The key conclusion has shifted away from a DDS amplitude-collapse problem and
toward a spectral-quality and closure problem.

What is now settled:

1. The early MSO22 “tone disappears above ~290 MHz” story is no longer the
   leading explanation.
2. The FSH8 evidence shows mild DDS-band droop, not collapse, through the
   previously suspected region.
3. Steady-state SFDR is now repeatable enough to treat as a real baseline,
   though it is still far below the long-term target.
4. UART throughput and RTT baselines are established.
5. Boot behavior is repeatable after tune, but not clean from init.
6. Scheduler-native preload and stream control paths are bench-smoked.

What is still open:

1. Acceptance-grade spectral quality, especially persistent spur-family root
   cause analysis.
2. Full close-in phase-noise characterization.
3. Deterministic dynamic settling / retune benchmarking.
4. Multi-channel timing and coherence evidence.
5. MSO22 timing automation and scope-based latency closure.

## Current Baseline Of Record

The frozen baseline remains [20260402T032626Z](../capture_runs/20260402T032626Z/).
It is still the canonical reference for the legacy benchmark flow.

Latest baseline-of-record results:

1. DDS-band: mild droop only, with the carrier present through the previously
   questioned region.
2. SFDR: structured nonzero values from about `48.6 dBc` to `59.96 dBc`.
3. Throughput: `axi_mmio_write = 736,980 ops/s`, `ad9144_spi_write = 5,945 ops/s`,
   `dds_pair_update = 838 ops/s`.
4. UART RTT: average around `3.46 ms`, with no giant outliers in the later run.

The follow-up SFDR confirmation run [sfdr_rerun](../capture_runs/sfdr_rerun/)
reproduced the same dominant spur family and stayed close to the frozen
baseline, so the SFDR baseline is mechanically trustworthy even though it is
not yet target-grade.

## Latest Results By Benchmark Family

### DDS Band

The current interpretation is that DDS output remains present and frequency
correct across the formerly suspicious `230-330 MHz` region. The best summary
remains the frozen DDS-band table in [Benchmark Results And History](./BENCHMARK_RESULTS_AND_HISTORY.md).

Main takeaway:

1. The amplitude response is not flat.
2. The tone does not collapse.
3. The earlier MSO22 result is now best treated as a measurement artifact.

### SFDR

The frozen steady-state SFDR baseline is still the reference.
It shows a repeatable, structured spur pattern with the worst case around
`48.6 dBc` at `400 MHz` in the baseline run.

What the evidence says now:

1. The SFDR flow works and is repeatable.
2. The dominant spur family reproduces in confirmation runs.
3. The numbers are still below the long-term `85-90 dBc` target.
4. The persistent `50 MHz -> ~729 MHz` spur remains unexplained enough to keep
   on the active problem list.

### Throughput And UART RTT

These are already closed as baseline measurements.

1. Raw AXI MMIO writes are much faster than SPI or DDS retunes.
2. DDS retune throughput is the slowest of the three firmware benchmarks.
3. UART RTT is in the low single-digit millisecond range on the cleaner run.

### Boot Repeatability

The restart story has split into two useful facts:

1. [boot_repeatability_20260404T165206Z](../capture_runs/boot_repeatability_20260404T165206Z/) shows stable recovery after tune.
2. [boot_repeatability_clean_init_rerun](../capture_runs/boot_repeatability_clean_init_rerun/) is the stronger negative result: all 5 cycles required tune, and post-tune latency did not collapse to a single deterministic signature.

Current interpretation:

1. “recovers cleanly after tune” is supported.
2. “clean from init without tune” is not supported.
3. “deterministic after tune” is not supported on the current build.

### Phase Noise

Raw-trace phase-noise capture remains blocked on the current FSH8 V1.58 trace
export path.

The later raw probe artifacts confirm that status rather than changing it:

1. [fsh_trace_debug/fsh_trace_probe_200mhz.json](../capture_runs/fsh_trace_debug/fsh_trace_probe_200mhz.json) reports `trace_success = false` and explains that the legacy firmware rejects the required trace-export SCPI path.
2. [phase_noise_trace_binary_r4/summary.json](../capture_runs/phase_noise_trace_binary_r4/summary.json) also shows the binary trace path in the same blocked state.

What is usable now is the marker-only offset method:

1. [phase_noise_offset_400mhz](../capture_runs/phase_noise_offset_400mhz/) produced a preliminary `400 MHz` offset survey.
2. [phase_noise_offset_400mhz_r2](../capture_runs/phase_noise_offset_400mhz_r2/) confirmed the keepable points.
3. The current reduced baseline is about `-101.3 dBc/Hz` at `10 kHz` and `-106.8 dBc/Hz` at `100 kHz`.
4. The `1 kHz` point is too close to the carrier skirt to keep as a claim.

This is a useful reduced baseline, but it is still not a full paper-grade curve.

### Dynamic Retune / Dynamic SFDR

The dynamic path is mechanically implemented and benchmarked, but the current
results are not repeatable enough to freeze as a strong baseline.

What happened:

1. The original `2 MHz` guard-band runs were mechanically useful but unstable.
2. [dynamic_sfdr_run_guard10mhz](../capture_runs/dynamic_sfdr_run_guard10mhz/) exposed a host-side overlap bug.
3. [dynamic_sfdr_run_guard10mhz_fixed](../capture_runs/dynamic_sfdr_run_guard10mhz_fixed/) is the first valid widened-guard result.
4. Later corrected reruns still split across different dominant endpoints and very different margins.

Current interpretation:

1. The method works.
2. The method does not converge repeatably enough on this bench.
3. The current dynamic result should be treated as closed with a non-repeatable outcome, not as an acceptance-grade settling claim.

### Scheduler-Native Benchmarking

This is the newest and most important addition to the benchmark story.

There are now two scheduler-native evidence tracks:

1. Preload-based dense/per-step FSH coverage.
2. Stream bring-up plus coarse stream-backed RF smoke.

Accepted preload/per-step smoke:

1. [fsh_scheduler_preload_perstep_200_210_rf_enforced](../capture_runs/fsh_scheduler_preload_perstep_200_210_rf_enforced/)
2. `loaded=11`, `commit=11`, `done=1`, `error=0`
3. RF quality passed with about `495 kHz` max frequency error, about `2.98 dB` flatness, and about `9.85 dB` max peak-vs-marker delta.

Accepted stream evidence:

1. [stream_bringup](../capture_runs/stream_bringup/)
2. `STREAM_DEPTH=511`
3. `STREAM_PUSHES=32`, `commit=32`, `free_space + occupancy = STREAM_DEPTH`
4. bad CRC was rejected, EOF completed, and the run ended cleanly with `done=true` and `error=false`.

Accepted stream-backed RF smoke:

1. [fsh_scheduler_stream_perstep_200_210_rf_enforced](../capture_runs/fsh_scheduler_stream_perstep_200_210_rf_enforced/)
2. It passed the same dense `200-210 MHz` per-step check through the stream transport.
3. The result is coarse RF smoke, not throughput proof and not dense `10 kHz` characterization.

## Remaining Gaps And Problems

### Instrument And Path Problems

1. The FSH8 V1.58 trace-export path is still blocked.
2. The legacy wide trace readout can time out or flatten to floor-level marker values.
3. The current dense stream path is correctness-smoked, but not yet a dense `10 kHz` characterization transport.
4. The MSO22 is not yet automated for timing truth.

### Measurement Problems

1. SFDR still needs spur-family root-cause analysis.
2. Dynamic retune evidence is not repeatable enough to support a strong closure claim.
3. The clean-init / deterministic-after-tune story is negative on the current build.
4. Channel skew and coherence cannot be closed with the current single-input analyzer setup.

### Operational Problems Seen In Later Full-Suite Attempts

The later `full_suite_*` directories are not durable benchmark evidence; they
are log-only or aborted runs. The logs show timeouts waiting for the old NCO
prompt, and at least one run surfaces PHY PRBS errors during the legacy prompt
flow. Those attempts are useful as failure evidence, but they do not replace
the accepted baseline runs.

## What Is Still Implemented But Not Yet Closed

1. Scheduler batch handling for the current `max_events=64` image.
2. UARTLite `STREAMHEX` bring-up and the supporting frame parser.
3. Full-sweep max-hold artifact generation with FSH8 guard rails.
4. RF path correction metadata support.
5. Scope-plan emission for future MSO22 timing work.
6. The legacy prompt-driven benchmark suite wrapper.

## Strategy To Close The Benchmark Work

The cleanest path is to stop spending cycles on already-settled claims and move
to the remaining blockers in a strict order.

1. Keep [Baseline Freeze](./baseline_freeze/README.md) as the canonical baseline and stop re-litigating DDS-band existence.
2. Treat the SFDR baseline as real, then spend time on spur-family root cause and calibration rather than more blind reruns.
3. Replace the blocked FSH8 trace-export path with either a working firmware/export sequence or a different instrument path that can support a real phase-noise curve.
4. If the dynamic settling claim still matters, switch from asynchronous max-hold reruns to a synchronized or gated measurement method.
5. Use the scheduler-native stream path to grow from correctness smoke into denser RF coverage only after the readout problem is solved.
6. Build the MSO22 timing suite only after marker routing is verified and a real observable is available on the board.
7. Keep RF calibration metadata attached to future runs so absolute-power claims can eventually be defended instead of inferred.

## Practical Next Work

1. Root-cause the persistent SFDR spur family.
2. Resolve or replace the blocked FSH8 trace-export path.
3. Decide whether the dynamic-retune claim is still worth closing; if yes, change the capture method.
4. Validate the MSO22 timing observables and implement the scope automation.
5. Extend stream regressions for low-watermark re-trigger, mode-locked behavior, reset flush, and underrun recovery.
6. Revisit dense `10 kHz` stream RF characterization only after the readout path is dependable.

## Bottom Line

The benchmark work is no longer blocked by whether the DDS path exists.
That question is settled.

The remaining work is about measurement quality, repeatability, and the last
missing validation paths: SFDR root cause, close-in phase noise, dynamic
settling, and timing automation.