# Benchmark Results And History

Date: 2026-04-05

This note collects the measurement history that led to the current working
conclusion.

## Measurement Story

The evaluation has had two distinct phases:

1. Scope-based investigation on the Tek MSO22
2. Spectrum-analyzer-based investigation on the R&S FSH8

The biggest conclusion change happened between those two phases.

## Phase 1: MSO22 Scope Result

Original observation on the MSO22:

1. waveform amplitude started to look weak around `250-270 MHz`
2. waveform was reported as effectively gone above `~290 MHz`

Important later conclusion:

1. the MSO22 bench unit had only `200 MHz` bandwidth
2. later analyzer measurements did not reproduce the severe collapse
3. that earlier result is now treated as a measurement artifact, not a current
   blocker on the DDS path

## Phase 2: FSH8 Analyzer Runs

### Run `20260402T011521Z`

Artifacts: not retained (superseded by later runs).

What it showed:

1. DDS-band droop was mild, not catastrophic
2. NCO translated tone still looked weak
3. SFDR, throughput, and UART RTT were not yet captured in that run

Interpretation:

1. this was the first strong evidence that the MSO22 result was misleading

### Run `20260402T012530Z`

Artifacts: not retained (superseded by later runs).

What it showed:

1. essentially the same DDS-band result as `20260402T011521Z`
2. still no full SFDR/throughput/UART coverage

Interpretation:

1. strengthened the conclusion that DDS-band amplitude collapse was not real in
   the way the scope suggested

### Run `20260402T022105Z`

Artifacts: not retained (superseded by `20260402T032626Z`).

What it showed:

1. first complete run covering:
   - DDS-band
   - SFDR
   - throughput
   - UART RTT
2. DDS-band still looked healthy
3. throughput and UART RTT were usable
4. SFDR was invalid because every result came out as `0 dBc`

Why SFDR was invalid:

1. the host logic reacquired the carrier as the spur
2. then a follow-on attempt to force wideband trace capture hit
   `TRAC:DATA? TRACE1` timeouts on the FSH8

Interpretation:

1. DDS-band, throughput, and UART RTT were already useful
2. SFDR automation still needed work

### Run `20260402T032626Z`

Artifacts:

1. [summary.json](../capture_runs/20260402T032626Z/summary.json)
2. [sfdr_results.csv](../capture_runs/20260402T032626Z/sfdr_results.csv)
3. [throughput.json](../capture_runs/20260402T032626Z/throughput.json)
4. [uart_rtt.json](../capture_runs/20260402T032626Z/uart_rtt.json)
5. [uart.log](../capture_runs/20260402T032626Z/uart.log)

This remains the current primary baseline and is frozen in
[Baseline Freeze](./baseline_freeze/README.md).

## Phase 3: Confirmation Runs (2026-04-04)

### Steady-state SFDR confirmation: `sfdr_rerun`

Artifacts:

1. [summary.json](../capture_runs/sfdr_rerun/summary.json)
2. [sfdr_results.csv](../capture_runs/sfdr_rerun/sfdr_results.csv)

What it showed:

1. the same dominant spur family reappeared at all eight carrier frequencies
2. the steady-state SFDR values stayed close to the `20260402T032626Z`
   baseline
3. the current bench setup was stable enough to trust the steady-state SFDR
   story as a real baseline

Interpretation:

1. steady-state SFDR is now trustworthy enough to use as a bench baseline
2. the result is still far below the long-term target, so the main RF-quality
   issue remains real
3. the `50 MHz` spur near `729 MHz` is now repeatable, but still not fully
   explained

### Restart / latency confirmation: `boot_repeatability_20260404T165206Z`

Artifacts:

1. [boot_repeatability.json](../capture_runs/boot_repeatability_20260404T165206Z/boot_repeatability.json)
2. [boot_repeatability.csv](../capture_runs/boot_repeatability_20260404T165206Z/boot_repeatability.csv)

What it showed:

1. all 5 cycles reached a clean final `SYSREF_STATUS`
2. every cycle required `fixed_by_tune`
3. all 5 cycles ended with the same latency signature:
   `0x03/0x03/0x0A/0x0A`
4. the aggregate verdict was `STABLE_AFTER_TUNE`

Interpretation:

1. restart behavior is repeatable after tune on the current build
2. this is good enough to support the current working baseline
3. it is not yet a "clean from init, no tune required" closure

## Phase 4: Remaining-Blocker Runs (2026-04-05)

### Marker-only phase-noise offset survey: `phase_noise_offset_400mhz` and `phase_noise_offset_400mhz_r2`

Artifacts:

1. [summary.json](../capture_runs/phase_noise_offset_400mhz/summary.json)
2. [phase_noise_offset_results.csv](../capture_runs/phase_noise_offset_400mhz/phase_noise_offset_results.csv)

What it showed:

1. the new marker-only offset method works on the current FSH8 bench
2. at `400 MHz`, the averaged sideband levels came out near:
   - `-33.2 dBc/Hz` at `1 kHz`
   - `-101.3 dBc/Hz` at `10 kHz`
   - `-106.8 dBc/Hz` at `100 kHz`
3. the confirmation rerun stayed within about `0.5 dB` at `10 kHz` and
   `0.7 dB` at `100 kHz`

Interpretation:

1. the `10 kHz` and `100 kHz` points are usable as a preliminary offset survey
2. the `1 kHz` point is too close to the carrier skirt to treat as trustworthy
3. this closes "can the current bench produce reduced close-in offset data?"
   for the current FSH8 bench
4. it still does not close "do we have a full paper-grade phase-noise curve?"

### Clean-from-init repeatability: `boot_repeatability_clean_init` and `boot_repeatability_clean_init_rerun`

Artifacts:

1. [boot_repeatability.json](../capture_runs/boot_repeatability_clean_init/boot_repeatability.json)
2. [boot_repeatability.csv](../capture_runs/boot_repeatability_clean_init/boot_repeatability.csv)
3. [boot_repeatability.json](../capture_runs/boot_repeatability_clean_init_rerun/boot_repeatability.json)
4. [boot_repeatability.csv](../capture_runs/boot_repeatability_clean_init_rerun/boot_repeatability.csv)

What it showed:

1. direct pre-tune SYSREF and latency capture is now working
2. valid cycles came up pre-tune with `SYSREF_STATUS = 0x00000003`
3. those cycles required tune to reach clean post-tune `0x00000001`
4. the latency signature was not fully deterministic across the run:
   - `0x03/0x03/0x0A/0x0A`
   - `0x02/0x02/0x0A/0x0A`
5. the first clean-init run had a malformed cycle 1 and was repeated
6. the replacement rerun captured a valid 5/5 table
7. all 5 rerun cycles still required tune, and post-tune latency still split
   across:
   - `0x03/0x03/0x0A/0x0A`
   - `0x02/0x02/0x0A/0x0A`

Interpretation:

1. the stronger clean-from-init claim is currently not supported
2. the replacement rerun also weakens the stronger deterministic-after-tune
   latency claim
3. the current build is best described as "recovers cleanly after tune," not as
   deterministic from init or deterministic after tune

### Dynamic retune repetition: `dynamic_sfdr_run`, `dynamic_sfdr_run_r2`, `dynamic_sfdr_run_r3`, `dynamic_sfdr_run_r4`, `dynamic_sfdr_run_guard10mhz`, `dynamic_sfdr_run_guard10mhz_fixed`, `dynamic_sfdr_run_guard10mhz_fixed_r2`, `dynamic_sfdr_run_guard10mhz_fixed_r3`, `dynamic_sfdr_run_guard10mhz_fixed_r4`, `dynamic_sfdr_run_guard10mhz_fixed_r5`, and `dynamic_sfdr_run_guard10mhz_fixed_r6`

Artifacts:

1. [dynamic_sfdr_results.csv](../capture_runs/dynamic_sfdr_run/dynamic_sfdr_results.csv)
2. [dynamic_sfdr_results.csv](../capture_runs/dynamic_sfdr_run_r2/dynamic_sfdr_results.csv)
3. [dynamic_sfdr_results.csv](../capture_runs/dynamic_sfdr_run_r3/dynamic_sfdr_results.csv)
4. [dynamic_sfdr_results.csv](../capture_runs/dynamic_sfdr_run_r4/dynamic_sfdr_results.csv)
5. [dynamic_sfdr_results.csv](../capture_runs/dynamic_sfdr_run_guard10mhz/dynamic_sfdr_results.csv)
6. [dynamic_sfdr_results.csv](../capture_runs/dynamic_sfdr_run_guard10mhz_fixed/dynamic_sfdr_results.csv)

What they showed:

1. the `2 MHz`-guard runs showed that the method worked mechanically, but
   endpoint dominance was still sensitive to near-endpoint energy
2. the first widened-guard attempt,
   [`dynamic_sfdr_run_guard10mhz`](../capture_runs/dynamic_sfdr_run_guard10mhz/),
   exposed a host-side overlap bug: the intended windows widened to `10 MHz`,
   but the unintended spur search still excluded only `2 MHz`
3. the corrected widened-guard run,
   [`dynamic_sfdr_run_guard10mhz_fixed`](../capture_runs/dynamic_sfdr_run_guard10mhz_fixed/),
   is the first valid widened-guard result:
   - `1 ms` favored `100 MHz` with `18.9 dB` guarded margin
   - `10 ms` favored `100 MHz` with `46.5 dB` guarded margin
4. later corrected reruns remained split:
   - `fixed_r2`: `1 ms -> 400 MHz / 40.6 dB`, `10 ms -> 400 MHz / 12.1 dB`
   - `fixed_r3`: `1 ms -> 100 MHz / 45.4 dB`, `10 ms -> 400 MHz / 12.9 dB`
   - `fixed_r4`: `1 ms -> 100 MHz / 45.7 dB`, `10 ms -> 100 MHz / 51.9 dB`
   - `fixed_r5`: `1 ms -> 100 MHz / 48.6 dB`, `10 ms -> 100 MHz / 1.5 dB`
   - `fixed_r6`: `1 ms -> 400 MHz / 40.4 dB`, `10 ms -> 100 MHz / 36.8 dB`
5. before the widened-guard correction, the `10 ms` case had already remained
   unsettled:
   - first run favored `100 MHz` at `18.3 dB`
   - second run favored `400 MHz` at `42.4 dB`
   - third run favored `100 MHz` at `52.4 dB`
   - fourth run favored `400 MHz` at `49.1 dB`

Interpretation:

1. the dynamic method is real and mechanically useful for exploratory work
2. [`dynamic_sfdr_run_guard10mhz`](../capture_runs/dynamic_sfdr_run_guard10mhz/)
   should be treated as a bug-finding run, not as final evidence
3. the widened-guard bug is fixed, but the corrected method still does not
   converge to one stable endpoint-dominance or margin story across repeated
   reruns
4. on the current asynchronous FSH8 max-hold bench, dynamic SFDR should
   therefore be treated as closed with a non-repeatable result rather than left
   pending for more blind repetitions

## DDS-Band Result Summary

### Latest baseline: `20260402T032626Z`

Relative to the `10 MHz` reference:

| Frequency | Delta |
|-----------|-------|
| 100 MHz | -0.166 dB |
| 200 MHz | -0.587 dB |
| 230 MHz | -0.818 dB |
| 240 MHz | -1.185 dB |
| 250 MHz | -1.510 dB |
| 260 MHz | -1.621 dB |
| 270 MHz | -1.552 dB |
| 280 MHz | -1.360 dB |
| 290 MHz | -1.244 dB |
| 300 MHz | -1.422 dB |
| 310 MHz | -1.889 dB |
| 320 MHz | -2.423 dB |
| 330 MHz | -2.768 dB |

Interpretation:

1. DDS-band amplitude is not flat, but it is far from "collapsed"
2. carrier frequency remains correct through the whole test region
3. this is consistent with a functioning DDS path and inconsistent with the
   original scope-based disappearance claim

## SFDR Result Summary

### Latest baseline: `20260402T032626Z`

From [sfdr_results.csv](../capture_runs/20260402T032626Z/sfdr_results.csv):

| Carrier | Carrier Power | Worst Spur | Spur Frequency | SFDR |
|---------|---------------|------------|----------------|------|
| 50 MHz | -1.787 dBm | -61.743 dBm | 729.143 MHz | 59.956 dBc |
| 100 MHz | -2.166 dBm | -60.212 dBm | 300.130 MHz | 58.046 dBc |
| 150 MHz | -2.195 dBm | -57.565 dBm | 449.473 MHz | 55.370 dBc |
| 200 MHz | -2.591 dBm | -60.028 dBm | 601.000 MHz | 57.437 dBc |
| 250 MHz | -3.504 dBm | -58.698 dBm | 749.479 MHz | 55.194 dBc |
| 300 MHz | -3.416 dBm | -57.289 dBm | 900.286 MHz | 53.873 dBc |
| 350 MHz | -4.505 dBm | -56.891 dBm | 700.686 MHz | 52.386 dBc |
| 400 MHz | -5.659 dBm | -54.251 dBm | 799.715 MHz | 48.592 dBc |

Interpretation:

1. the SFDR flow is now producing structured, nonzero results
2. many detected worst spurs are harmonic-looking and therefore at least
   physically plausible
3. the measured baseline is still well below the long-term target of
   `>= 85-90 dBc`
4. this is now the main RF-quality issue to refine

Cautions:

1. these values should still be treated as preliminary bench baselines
2. the baseline is now supported by a confirmation rerun under fixed settings,
   but not yet by a root-cause isolation campaign
3. the `50 MHz` case in particular still deserves a sanity check because the
   reported worst spur near `729 MHz` is less obviously aligned with the simple
   harmonic pattern seen in the other rows

## Throughput Baseline Summary

### Run `20260402T022105Z`

1. `axi_mmio_write`: `736,980 ops/s`
2. `ad9144_spi_write`: `5,945 ops/s`
3. `dds_pair_update`: `838 ops/s`

### Run `20260402T032626Z`

1. `axi_mmio_write`: `736,980 ops/s`
2. `ad9144_spi_write`: `5,945 ops/s`
3. `dds_pair_update`: `838 ops/s`

Interpretation:

1. the throughput baseline is stable across the two full runs
2. SPI and DDS retune paths are much slower than raw AXI MMIO

## UART RTT Baseline Summary

### Run `20260402T022105Z`

1. min `3161.9 us`
2. avg `9128.9 us`
3. max `50633.1 us`
4. two large outliers near `50 ms`

### Run `20260402T032626Z`

1. min `3170.6 us`
2. avg `3455.8 us`
3. max `5125.6 us`
4. no giant outliers

Interpretation:

1. typical RTT is roughly low-single-digit milliseconds
2. the later run looks like the cleaner baseline to keep

## Scheduler Benchmark Summary

### Stream correctness smoke

Artifact:
[stream_bringup](../capture_runs/stream_bringup/)

Result:

1. `STREAM_DEPTH = 511`
2. bad CRC rejected without changing accepted stream counters
3. one-event EOF stream reached `STREAM_PUSHES=1`, `commit=1`, `eof_seen=1`,
   `done=1`, `error=0`
4. 32-event refill run reached `STREAM_PUSHES=32`, `commit=32`,
   `free_space=511`, and `free_space + occupancy == STREAM_DEPTH`

Interpretation:

1. UARTLite stream mode is bench-smoked as a correctness and observability
   transport
2. UARTLite is not a throughput transport; dense-stream stress remains deferred
   to UART16550, Ethernet, or another higher-rate path

### Scheduler preload RF smoke

Artifact:
[fsh_scheduler_preload_perstep_200_210_rf_enforced](../capture_runs/fsh_scheduler_preload_perstep_200_210_rf_enforced/)

Result:

1. scheduler execution: `loaded=11`, `commit=11`, `done=1`, `error=0`
2. RF quality: `passed=true`
3. max frequency error: about `495 kHz`
4. flatness: about `2.98 dB`
5. max peak-vs-marker delta: about `9.85 dB`

Interpretation:

1. preload scheduler plus per-step FSH8 measurement is now the accepted
   scheduler RF coverage smoke path
2. this is coarse RF coverage evidence, not precision spectral validation
3. full-sweep FSH max-hold remains an artifact path only on FSH8 `V1.58`,
   because trace export fails and marker fallback can flatline at the floor

## Current Historical Conclusion

Across the current measurement history:

1. the DDS-band "collapse" story has been effectively refuted by the FSH8 and
   is now frozen in the baseline-of-record
2. throughput and UART latency baselines are established
3. steady-state SFDR is now confirmed as a trustworthy bench baseline
4. clean recovery after tune is repeatable, but deterministic latency after
   tune is not yet closed
5. the current FSH8 can now produce preliminary close-in offset data through
   the marker-only phase-noise method
6. clean-from-init behavior has now been tested directly and is currently a
   negative result on this build
7. dynamic retune evidence is now good enough to keep as an exploratory method,
   but the corrected widened-guard reruns remain non-repeatable, so the current
   bench does not support freezing a strong dynamic-settling claim
8. channel skew/coherence remains open and out of scope on the single-input
   FSH8
9. NCO has become secondary to the main DDS evaluation path
10. scheduler preload execution, UARTLite stream correctness, and coarse
    stream RF smoke are now bench-smoked; dense `10 kHz` stream RF
    characterization and MSO22 timing automation are the next scheduler
    milestones
