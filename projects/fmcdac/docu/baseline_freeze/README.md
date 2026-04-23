# FMCDAC Baseline Freeze

Date: 2026-04-05

This folder defines the single current baseline-of-record for the FMCDAC
evaluation and the confirmation artifacts that support it.

## Baseline Of Record

The canonical baseline remains:

1. [`20260402T032626Z`](../../capture_runs/20260402T032626Z/)

Use that run as the project baseline unless there is an explicit review to
replace it.

## Baseline Platform And Configuration

Current frozen baseline configuration:

1. AD9144-FMC-EBZ on KCU116
2. JESD204B mode 4 (`M=2, L=4, F=1, S=1`)
3. Subclass 1
4. DAC PLL at `1966.08 MSPS`
5. `2x` interpolation enabled
6. FPGA/JESD input rate `983.04 MSPS`
7. External `122.88 MHz` reference into AD9516
8. Continuous `30.72 MHz` SYSREF
9. Rising-edge SYSREF at init
10. R&S FSH8 as the reference RF instrument

## Frozen Artifacts

The baseline-of-record artifact set is:

1. [`summary.json`](../../capture_runs/20260402T032626Z/summary.json)
2. [`sfdr_results.csv`](../../capture_runs/20260402T032626Z/sfdr_results.csv)
3. [`throughput.json`](../../capture_runs/20260402T032626Z/throughput.json)
4. [`uart_rtt.json`](../../capture_runs/20260402T032626Z/uart_rtt.json)
5. [`uart.log`](../../capture_runs/20260402T032626Z/uart.log)

## Settled Baseline Conclusions

These statements are now baseline truth for the current configuration:

1. DDS output survives through the previously suspect `230-330 MHz` region.
2. The earlier MSO22 amplitude-collapse story is treated as a measurement
   artifact.
3. Throughput and UART RTT baselines exist and are stable enough to retain.
4. Steady-state SFDR is measurable and structured from `50-400 MHz`, but well
   below the long-term target.
5. NCO is no longer the primary diagnostic path.

## Confirmation Artifacts

These runs confirm the current baseline story without replacing the baseline of
record:

1. [`sfdr_rerun`](../../capture_runs/sfdr_rerun/)
   - confirms the steady-state SFDR baseline under fixed analyzer settings
   - reproduces the same dominant spur family across all tested carriers
2. [`boot_repeatability_20260404T165206Z`](../../capture_runs/boot_repeatability_20260404T165206Z/)
   - confirms `STABLE_AFTER_TUNE` behavior across 5 boot cycles
   - shows one stable latency signature after tune

## Not Yet Settled

These items are still open and must not be presented as closed:

1. Acceptance-grade SFDR or SFDR root-cause attribution
2. Phase-noise closure near the intended operating band
   - marker-only offset measurements are available as the current FSH8 method
   - close-in trace capture is blocked on the current FSH8 `V1.58` firmware
   - acceptance-grade phase-noise evidence is still open until dense trace
     export or a better instrument path exists
3. Dynamic SFDR / settling during rapid retunes or chirps
4. Clean-from-init deterministic latency without post-link tune
5. Channel skew / coherence evidence under simultaneous updates
   - deferred until a multi-channel RF instrument is available

## Usage Rule

Future runs should be compared against this folder first.

Do not replace the `20260402T032626Z` baseline-of-record merely because a later
confirmation run exists. Replace it only after explicit review.
