# Automation And Implementation Status

Date: 2026-05-20

This note captures the current implementation state of the firmware diagnostics
and the host automation used to benchmark the FMCDAC platform.

## Main Code Files

Primary firmware files:

1. [fmcdac.c](../src/app/fmcdac.c)
2. [ad9144.c](../../../drivers/dac/ad9144/ad9144.c)

Primary host tools:

1. [run_nco_scope_test.py](../run_nco_scope_test.py)
2. [capture_boot_repeatability.py](../capture_boot_repeatability.py)
3. [dds_band_plot.py](../dds_band_plot.py)
4. [plot_dds_band_summary.py](../plot_dds_band_summary.py)

Primary docs:

1. [Baseline Freeze](./baseline_freeze/README.md)
2. [CURRENT_EVALUATION_STATUS.md](./CURRENT_EVALUATION_STATUS.md)
3. [BENCHMARK_RESULTS_AND_HISTORY.md](./BENCHMARK_RESULTS_AND_HISTORY.md)
4. [PRIOR_EXPLORATIONS_AND_ARCH_OPTIONS.md](./PRIOR_EXPLORATIONS_AND_ARCH_OPTIONS.md)
5. [NCO_SCOPE_AUTOMATION.md](./NCO_SCOPE_AUTOMATION.md)

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
4. `DYNAMIC-SFDR` paused diagnostic:
   - `100 MHz <-> 400 MHz` retune burst with `1 ms` dwell
   - `100 MHz <-> 400 MHz` retune burst with `10 ms` dwell
5. `THROUGHPUT` benchmark:
   - raw AXI MMIO writes
   - AD9144 SPI writes
   - DDS pair retunes
6. `UART-RTT` ping/pong service for host-side timing

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
4. uploaded AWG scheduler UART console flow with host-built event tables
5. optional per-step FSH8 validation during uploaded AWG scheduler runs
6. FSH8 measurement flow for:
   - DDS-band
   - SFDR
   - close-in carrier traces during selected SFDR steps
   - dynamic retune bursts
   - throughput collection
   - UART RTT collection
7. scheduler-native benchmark suite path:
   - `--run-scheduler-benchmark-suite`
   - host-side batching over the current scheduler event-depth limit
   - FSH dense stepped-sweep validation
   - dense RF quality summaries and optional enforcement
   - FSH scheduler-held SFDR spot validation
   - full-sweep max-hold artifact generation with FSH8 V1.58 guard rails
   - UARTLite `stream-bringup` correctness profile
   - emitted `scheduler_scope_plan.json` for future MSO22 timing execution
8. two-pass `--run-full-integration` wrapper:
   - pass 1: measured uploaded AWG scheduler sweep
   - pass 2: legacy paused DDS-band / SFDR / dynamic / throughput / UART-RTT suite
9. artifact generation into `capture_runs/<timestamp>/`

### Current role of `capture_boot_repeatability.py`

Implemented:

1. repeated boot capture over UART
2. parsing of:
   - `SYSREF-TUNE`
   - `SYSREF_STATUS`
   - `LATENCY`
3. aggregate verdict output:
   - `PASS`
   - `STABLE_AFTER_TUNE`
   - `REVIEW`
4. artifact generation for multi-boot repeatability evidence

### Current workflow policy

Primary path for baseline reproduction:

1. DDS-band
2. SFDR
3. throughput
4. UART RTT

Secondary path:

1. NCO is opt-in only via `--run-nco-test`

Current decision-driving path:

1. hold the frozen baseline in [Baseline Freeze](./baseline_freeze/README.md)
2. use `sfdr_rerun` only to confirm steady-state SFDR, not to replace the
   baseline-of-record
3. use `capture_boot_repeatability.py` to close restart / latency evidence
4. treat the raw-trace phase-noise method as blocked on the FSH8 trace-export
   path, but use the marker-only offset sweep for close-in offset evidence on
   the current bench
5. treat uploaded AWG scheduler validation as a separate deterministic
   control-plane + RF-validation path, not yet as a replacement for the whole
   legacy benchmark suite
6. spend remaining bench time on:
   - root-cause interpretation of the still-negative clean-init result only if
     that claim remains required
   - one more dynamic retune rerun with a wider intended guard if the `10 ms`
     case still needs settling

Current scheduler limitation:

1. the current KCU116 image reports `max_events=64`
2. dense one-shot sweeps such as `200-300 MHz` in `10 kHz` steps do not fit in
   one uploaded schedule
3. those benches currently use host-side batching through the legacy scheduler
   UART preload console
4. Phase A HDL stream FIFO support and Phase B firmware DDR-staged refill now
   exist behind `FMCDAC_AWG_SCHED_STREAM`
5. UARTLite `STREAMHEX` has passed correctness smoke, including bad CRC,
   finite EOF, reset reuse, `STREAM_DEPTH=511`, and a 32-event refill run
6. the host benchmark suite can now run finite stream-backed per-step dense
   FSH sweeps; bench acceptance of that path is the next run

Current accepted scheduler RF smoke:

1. path: scheduler-native preload `LOADBIN/RUN` plus per-step FSH8 captures
2. artifact:
   [fsh_scheduler_preload_perstep_200_210_rf_enforced](../capture_runs/fsh_scheduler_preload_perstep_200_210_rf_enforced/)
3. sweep: `200-210 MHz` in `1 MHz` steps
4. result: `loaded=11`, `commit=11`, `done=1`, `error=0`
5. RF quality: pass with max frequency error about `495 kHz`, flatness about
   `2.98 dB`, and max peak-vs-marker delta about `9.85 dB`
6. power calibration status: artifacts can record/apply fixed or
   frequency-dependent RF path correction, but current accepted smoke should
   still be treated as coarse relative RF evidence unless a calibration table
   and analyzer level-check artifact are supplied

Current FSH8 limitation:

1. full-sweep `maxhold` path is implemented and writes complete artifacts
2. on the current FSH8 `V1.58`, trace export fails and marker fallback can
   return floor-level flatlines
3. the host therefore marks those max-hold bins `marker_flatline_untrusted`
   and does not treat them as RF evidence

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
4. the `2026-04-04` confirmation rerun reproduced the same dominant spur family
   at all tested carriers under the frozen analyzer settings

Phase-noise scout traces:

1. `run_nco_scope_test.py` has the code path for close-in carrier traces during
   selected SFDR steps with `--phase-noise-span-hz`, but that path is blocked
   on the current FSH8 `V1.58` firmware
2. these traces reuse the paused SFDR tone holds, so no new firmware prompt is
   required
3. the resulting raw trace CSVs are intended to support the next phase-noise
   campaign near `~400 MHz`
4. this is still a bench-scout flow, not an acceptance-grade phase-noise claim
5. current status:
   - compatibility probing shows `FORM*`, memory-trace commands, and
     `TRAC:DATA? TRACE1` / `TRAC? TRACE1` are not usable on this `V1.58` unit
   - [`phase_noise_scout_retry_1mhz`](../capture_runs/phase_noise_scout_retry_1mhz/)
     degraded to a one-point fallback trace even with `1 MHz` span,
     `RBW/VBW = 10 kHz`, and `sweep_count = 1`
   - the host now fails fast for trace-based requests on legacy FSH8 firmware
   - treat the raw-trace method as blocked on this bench until firmware,
     syntax, or export method changes

Marker-only phase-noise offset sweep:

1. `run_nco_scope_test.py` can now measure exact left/right sideband levels at
   fixed offsets using `--phase-noise-offset-hz`
2. this path does not call `TRAC:DATA? TRACE1`; it uses marker reads only
3. the intended workflow is:
   - hold a steady SFDR carrier, typically `400 MHz`
   - measure left/right sidebands at fixed offsets
   - normalize the averaged sideband level by carrier power and RBW
4. the resulting reduced artifact is `phase_noise_offset_results.csv`
5. this is the current practical close-in phase-noise method on the FSH8
6. current status:
   - [`phase_noise_offset_400mhz`](../capture_runs/phase_noise_offset_400mhz/)
     produced a usable preliminary offset survey at `400 MHz`
   - [`phase_noise_offset_400mhz_r2`](../capture_runs/phase_noise_offset_400mhz_r2/)
     confirmed the same `10 kHz` and `100 kHz` points within about `0.5-0.7 dB`
   - the `10 kHz` and `100 kHz` points are now the keepable reduced baseline
   - the `1 kHz` point is too close to the carrier skirt to keep as a claim

Dynamic retune bursts:

1. `run_nco_scope_test.py` can now drive the paused `DYNAMIC-SFDR` prompt
2. the host starts each live burst, then measures:
   - intended-tone windows near `100 MHz` and `400 MHz`
   - the strongest unintended spur outside guarded windows
3. the current representative cases are `1 ms` and `10 ms` dwell toggles over
   the same `100 MHz <-> 400 MHz` pair
4. the results are summarized in `dynamic_sfdr_results.csv`
5. current status:
   - [`dynamic_sfdr_run`](../capture_runs/dynamic_sfdr_run/) is the first
     working artifact set for this path
   - repeated runs now exist in
     [`dynamic_sfdr_run_r2`](../capture_runs/dynamic_sfdr_run_r2/),
     [`dynamic_sfdr_run_r3`](../capture_runs/dynamic_sfdr_run_r3/), and
     [`dynamic_sfdr_run_r4`](../capture_runs/dynamic_sfdr_run_r4/)
   - [`dynamic_sfdr_run_guard10mhz`](../capture_runs/dynamic_sfdr_run_guard10mhz/)
     showed that widening only the intended windows was not enough; the host
     still excluded only `2 MHz` from the spur search
   - the host now excludes the full intended margin from the unintended search
     windows
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
     margins
   - the corrected widened-guard method is therefore mechanically correct but
     still not repeatable enough to freeze as a stable dynamic baseline on the
     current asynchronous FSH8 max-hold bench

Boot repeatability:

1. repeated boot cycles are captured over UART only
2. the tool records:
   - pre-tune `SYSREF_STATUS`
   - pre-tune latency signature
   - tune outcome
   - final `SYSREF_STATUS`
   - final `dyn0/dyn1/var0/var1` latency signature
3. current closure rule:
   - `STABLE_AFTER_TUNE` means every cycle reaches a clean post-tune state and
     one latency signature
   - `PASS` means no tune was required and one latency signature was observed
   - `clean_init_verdict = PASS` means every cycle was already clean before the
     tune logic ran
4. current status:
   - [`boot_repeatability_20260404T165206Z`](../capture_runs/boot_repeatability_20260404T165206Z/)
     is now historical evidence for clean recovery after tune
   - [`boot_repeatability_clean_init_rerun`](../capture_runs/boot_repeatability_clean_init_rerun/)
     is the current direct clean-init dataset
   - all 5 cycles in that rerun required tune from pre-tune
     `SYSREF_STATUS = 0x00000003` to reach `0x00000001`
   - post-tune latency still split across two signatures, so deterministic
     latency is not yet closed even after tune

## Artifact Status

### Produced by the current flow

Per run:

1. `summary.json`
2. `uart.log`
3. `dds_band_plot.csv`
4. `dds_band_plot.svg`
5. `sfdr_results.csv`
6. `phase_noise_results.csv`
7. `phase_noise_offset_results.csv`
8. `dynamic_sfdr_results.csv`
9. `throughput.json`
10. `uart_rtt.json`
11. `boot_repeatability.json`
12. `boot_repeatability.csv`

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

`phase_noise_results.csv`

1. reduced manifest of requested close-in carrier traces
2. points to the per-step raw trace CSVs that support the phase-noise scout run

`phase_noise_offset_results.csv`

1. reduced summary of the marker-only phase-noise offset sweep
2. reports left/right sideband power plus averaged sideband level in
   `dBc` and `dBc/Hz`

`dynamic_sfdr_results.csv`

1. reduced summary of the dynamic retune-burst results
2. reports intended-tone reference power, strongest unintended spur, and the
   guarded spur margin for each burst case

`throughput.json`

1. machine-readable baseline of firmware software update rates

`uart_rtt.json`

1. machine-readable baseline of host-to-firmware UART round-trip latency

`boot_repeatability.json`

1. machine-readable multi-boot SYSREF and latency summary
2. contains verdict, review reasons, latency signatures, and per-cycle records

`boot_repeatability.csv`

1. flat table of per-cycle SYSREF and latency signatures for quick comparison

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

1. phase-noise measurement near `~400 MHz`
   - raw trace export is blocked by FSH8 `V1.58` SCPI compatibility
   - a confirmed marker-only offset survey now exists, but it is still a
     reduced point-sample result rather than a full close-in curve
2. dynamic SFDR during rapid steps or chirps
   - the corrected widened-guard method now works mechanically
   - repeated corrected reruns show it is still state-sensitive / timing-
     sensitive on this bench
   - treat it as exploratory evidence rather than a closed paper-grade claim
3. clean-from-init no-tune SYSREF closure, if that stronger claim is required
   - the current direct measurement says the build does not yet pass that claim
   - deterministic-after-tune latency is also not yet closed
4. channel skew / coherence evidence under simultaneous updates
   - deferred until a multi-channel RF instrument is available
   - the single-input FSH8 cannot close this claim
5. optional HDL experiments if future evidence points back toward a digital
   issue

## Practical Next Steps

1. keep [Baseline Freeze](./baseline_freeze/README.md) and
   [20260402T032626Z](../capture_runs/20260402T032626Z/) as the current
   reference baseline
2. keep [sfdr_rerun](../capture_runs/sfdr_rerun/) as the SFDR confirmation run
3. keep
   [boot_repeatability_20260404T165206Z](../capture_runs/boot_repeatability_20260404T165206Z/)
   as the current repeatability-after-tune evidence
4. keep
   [phase_noise_scout_retry_1mhz](../capture_runs/phase_noise_scout_retry_1mhz/)
   as evidence that the current FSH8 close-in trace export path is blocked
5. keep
   [phase_noise_offset_400mhz](../capture_runs/phase_noise_offset_400mhz/)
   as the current preliminary close-in offset baseline on the FSH8
6. keep
   [boot_repeatability_clean_init_rerun](../capture_runs/boot_repeatability_clean_init_rerun/)
   as the authoritative clean-init evidence set for the current build

## TODOs / Blockers

1. FSH8 trace export:
   - ask R&S for the supported trace-export command sequence on FSH8 firmware
     `V1.58`, or upgrade firmware
   - rerun `fsh_trace_probe.py` after any firmware or syntax change
   - only re-enable `--phase-noise-span-hz` after `trace_success=true` with
     more than one trace point
2. Full phase-noise curve:
   - blocked until dense trace export or another phase-noise-capable
     instrument path exists
   - current marker-only offset data remains a reduced baseline, not a full
     curve
3. Dynamic SFDR:
   - blocked from paper-grade closure by asynchronous FSH8 max-hold capture
   - next useful work is synchronized/gated capture, not more identical reruns
4. Spectral quality:
   - steady-state SFDR is repeatable but below target
   - root-cause persistent spur families before making acceptance-grade claims
5. Clean init / deterministic latency:
   - current build is closed negative
   - further work requires a SYSREF/init-policy change, not more measurement
7. keep [dynamic_sfdr_run](../capture_runs/dynamic_sfdr_run/),
   [dynamic_sfdr_run_r2](../capture_runs/dynamic_sfdr_run_r2/), and
   [dynamic_sfdr_run_r3](../capture_runs/dynamic_sfdr_run_r3/) as the current
   dynamic-burst evidence set, with
   [dynamic_sfdr_run_r4](../capture_runs/dynamic_sfdr_run_r4/) confirming that
   a wider intended guard is needed for cleaner classification, and keep
   [dynamic_sfdr_run_guard10mhz_fixed](../capture_runs/dynamic_sfdr_run_guard10mhz_fixed/)
   as the first valid corrected widened-guard result
8. do not spend more time on identical `DYNAMIC-SFDR` reruns on this bench
   unless the capture method changes materially
9. defer channel skew/coherence until suitable multi-channel instrumentation is
   available
10. only revisit NCO or HDL-focused experiments if those measurements point back
   toward a digital explanation
