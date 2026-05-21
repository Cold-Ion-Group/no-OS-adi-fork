# Scheduler Handoff Status

Date: 2026-05-20

## Purpose

This note is the canonical handoff for the current scheduler, scope, and
benchmarking effort. It is intended to let another agent pick up the work
without reconstructing the thread history from UART logs, host code, and
multiple status documents.

It answers five questions:

1. what the final intended system-level outcome is
2. what has actually been bench-validated so far
3. what the latest archived runs prove
4. what remains implemented-but-unvalidated versus blocked
5. what the next engineering tasks are

## Current Update Snapshot

The previous handoff archived `manual_awg_run` as the first scheduler-native
evidence. The current status has advanced:

1. The active scheduler tick rate is `245760000`, not the older `100000000`
   value shown in early artifacts.
2. UARTLite stream bring-up is now bench-smoked for correctness:
   `STREAM_DEPTH=511`, bad CRC rejected, one-event EOF completes, and a
   32-event refill run reaches `STREAM_PUSHES=32` and `commit=32`.
3. FSH8 full-sweep `maxhold` is implemented but not accepted as RF evidence on
   the current FSH8 `V1.58` unit. Trace export fails and marker fallback can
   read flat floor values; the host marks these bins
   `marker_flatline_untrusted`.
4. The current accepted scheduler RF coverage smoke is preload/per-step FSH:
   `200-210 MHz @ 1 MHz`, `10 s` dwell, `scale_u=1200000`,
   `RBW/VBW=10 kHz`, FSH preamp on, and enforced RF quality with relaxed
   peak-vs-marker delta. Result: `loaded=11`, `commit=11`, `done=1`,
   `error=0`, `rf_quality.passed=true`, max frequency error about `495 kHz`,
   flatness about `2.98 dB`, and max peak-vs-marker delta about `9.85 dB`.

## Final Intended Outcome

The intended end state is not just "the scheduler runs." The intended end
state is a deterministic benchmark engine for the FMCDAC system that can:

1. execute frequency, amplitude, phase, and pulse schedules with low and
   predictable control overhead
2. replace prompt-driven interactive DDS stepping for dense RF characterization
3. support fine-granularity sweeps such as `200 MHz` to `300 MHz` in `10 kHz`
   steps without per-step host UART command overhead
4. support deterministic latency and pulse-width benchmarking with a scope
5. provide a reproducible measurement flow that separates:
   - RF spectral truth on the FSH8
   - timing and latency truth on the MSO22

The final intended validation scope is:

### FSH8 suite

Use the FSH8 to validate:

1. carrier frequency tracking versus programmed scheduler step
2. power flatness versus frequency
3. scheduler batch continuity summaries
4. scheduler-held steady-state SFDR spot checks
5. deterministic tone behavior during scheduled holds

### MSO22 suite

Use the MSO22 for timing, not RF spectral truth. The target scope suite is:

1. first-event latency after arm / epoch reload / run
2. hop or switch latency between scheduled events
3. minimum stable dwell and minimum usable pulse width
4. pulse spacing accuracy
5. preload-batch boundary gap characterization
6. correlation between scheduled event timing and a routed timing marker or
   trigger signal

## Architecture Status

### Scheduler control path

The scheduler control-plane path is working on the active bench image:

1. IP identity and capability registers are readable
2. `LOADBIN` upload works over the dedicated AWG UART console
3. event commit works
4. epoch reload works
5. scheduler execution works
6. the measured host path can align captures to the scheduler epoch

### Stream path

Stream status is:

1. Phase A HDL stream ABI is frozen and mirrored in the repo
2. Phase B firmware stream support is implemented and build-verified
3. stream support is gated by `FMCDAC_AWG_SCHED_STREAM=0` by default
4. the stream frame parser exists
5. UARTLite ASCII-hex stream ingress is implemented for smoke tests through
   `STREAMHEX <bytes>`
6. host tooling can pack stream frames, parse ACK/status lines, and run a
   `stream-bringup` profile
7. UARTLite stream mode is not a throughput target; it is for correctness,
   EOF, reset, refill, and soak/leak testing
8. the active FSH benchmark path is still legacy preload via `LOADBIN/RUN`

This means the current scheduler-native suite is scheduler-native relative to
the old paused prompt flow. A stream bring-up path now exists in code, but
dense RF benchmarking is not stream-native until that profile passes on
hardware.

### Event depth

The active image reports:

1. `max_events = 64`
2. `tick_hz = 245760000`

As a result, dense sweeps such as `200 MHz` to `300 MHz` in `10 kHz` steps do
not fit in one hardware preload table. Today they still require host-side
batching unless and until stream transport is wired and bench-validated.

Legacy preload direct comparisons are bounded by the HDL event BRAM address
width. Treat `EVENT_MEM_ADDR_WIDTH = 8` as a `256` event comparison ceiling:

1. `<=256` events can be compared directly in preload and stream modes
2. `>256` events are stream-only for full-program execution
3. full `10k` dense sweeps should use historical dense DDS and smaller preload
   subsets as baselines

The first stream transport is bandwidth-bounded by UARTLite:

1. `115200` baud is about `11.5 KB/s` raw before console overhead
2. ASCII hex doubles each `32` byte event to `64` wire characters
3. realistic sustained throughput is about `100-150 events/s`
4. `10,000` events take about `67-100 s` to stream
5. `100,000` events take about `11-17 min` and should be interpreted as a
   correctness/leak soak, not transport stress

## Latest Archived Runs

This handoff uses these archived runs as the latest concrete evidence:

1. legacy dense DDS / sparse SFDR / phase run:
   - [summary.json](/C:/Users/fpga_/yr/tmp/no-OS-adi-fork/projects/fmcdac/capture_runs/matrix12_dense_dds_sparse_sfdr_phase/summary.json)
2. scheduler-native manual benchmark run:
   - [scheduler_benchmark_suite.json](/C:/Users/fpga_/yr/tmp/no-OS-adi-fork/projects/fmcdac/capture_runs/manual_awg_run/scheduler_benchmark_suite.json)
   - [scheduler_dense_sweep.json](/C:/Users/fpga_/yr/tmp/no-OS-adi-fork/projects/fmcdac/capture_runs/manual_awg_run/dense_sweep/scheduler_dense_sweep.json)
   - [scheduler_sfdr_spot_set.json](/C:/Users/fpga_/yr/tmp/no-OS-adi-fork/projects/fmcdac/capture_runs/manual_awg_run/sfdr_spots/scheduler_sfdr_spot_set.json)
3. stream correctness smoke:
   - [scheduler_benchmark_suite.json](../capture_runs/stream_bringup/scheduler_benchmark_suite.json)
4. current accepted preload/per-step RF smoke:
   - [scheduler_benchmark_suite.json](../capture_runs/fsh_scheduler_preload_perstep_200_210_rf_enforced/scheduler_benchmark_suite.json)
5. RF power correction support:
   - host accepts `--rf-power-correction-db` and
     `--rf-power-calibration-csv`
   - artifacts preserve raw analyzer dBm and add corrected power fields when
     correction is enabled
6. finite stream-backed per-step dense FSH host path:
   - implemented for `--scheduler-transport stream`
   - next required bench result is comparison against the accepted preload
     `200-210 MHz` smoke

### 1. Legacy dense DDS run: `matrix12_dense_dds_sparse_sfdr_phase`

This run is an old-firmware, non-scheduler benchmark archive. It still matters
because it captures what the bench was already doing well before the scheduler
path existed.

What it contains:

1. dense DDS-band sweep from `200 MHz` to `300 MHz`
2. `10001` points, i.e. `10 kHz` stepping
3. sparse SFDR sweep over the same band with `5` points
4. phase-noise and offset-sideband captures enabled
5. analyzer settings:
   - `RBW = 100 kHz`
   - `VBW = 100 kHz`
   - `sweep_count = 3`
   - `trace_mode = average`

What it proves:

1. the FSH8-based host automation can already support dense RF characterization
   on the legacy DDS stepping path
2. the project already has prior art for `10 kHz`-granularity RF sweeps
3. sparse SFDR and phase-noise capture workflows already exist in the repo

What it does not prove:

1. it does not validate the scheduler path
2. it does not validate deterministic scheduler-driven event timing
3. it does not remove host command overhead from the dense sweep mechanism
4. it is not an absolute-power calibrated artifact unless an RF path correction
   table and analyzer level check are attached

This run is therefore best treated as the legacy RF characterization baseline,
not as evidence that the scheduler mission is complete.

### 2. Scheduler-native run: `manual_awg_run`

This is the first scheduler-native benchmark archive. It remains useful
history, but it has been superseded by the later stream smoke and enforced
preload/per-step RF smoke described in the update snapshot above.

Top-level configuration:

1. mode: `scheduler_benchmark_suite`
2. profile: `all`
3. scheduler console info:
   - `base_addr = 0x44AA0000`
   - `max_events = 64`
   - `tick_hz = 100000000` in this historical artifact
   - current active builds use `tick_hz = 245760000`
   - `timeout_ms = 2000`
   - `dds_clock_hz = 983056640`
   - `dds_phase_dw = 32`

#### 2a. Dense scheduler FSH sweep

The dense scheduler sweep archive shows:

1. `freq_count = 11`
2. `batch_count = 1`
3. one preload batch covering `200 MHz` through `210 MHz` in `1 MHz` steps
4. dwell per event: `2 s`
5. narrow-span FSH measurements centered on each expected tone
6. epoch-relative capture windows recorded in the JSON

What this proves:

1. the scheduler can execute a multi-event uploaded batch deterministically
2. the host can anchor FSH measurements to the scheduler epoch
3. the FSH can track the programmed stepped tones closely enough to validate
   the scheduler-driven RF output path

Observed frequency tracking from the archived run:

1. `200 MHz -> 200.065079 MHz` error `+65.079 kHz`
2. `201 MHz -> 201.482540 MHz` error `+482.540 kHz`
3. `202 MHz -> 202.398413 MHz` error `+398.413 kHz`
4. `203 MHz -> 203.490476 MHz` error `+490.476 kHz`
5. `204 MHz -> 204.257143 MHz` error `+257.143 kHz`
6. `205 MHz -> 205.420635 MHz` error `+420.635 kHz`
7. `206 MHz -> 206.317460 MHz` error `+317.460 kHz`
8. `207 MHz -> 206.561905 MHz` error `-438.095 kHz`
9. `208 MHz -> 208.331746 MHz` error `+331.746 kHz`
10. `209 MHz -> 208.717460 MHz` error `-282.540 kHz`
11. `210 MHz -> 210.460317 MHz` error `+460.317 kHz`

Interpretation:

1. this is a real scheduler-plus-FSH validation run
2. the FSH tracking is coherent with the programmed sequence
3. the residual error is still on the order of a few hundred kilohertz, not
   precision-metrology grade
4. the path is good enough to validate scheduler-driven stepped output
5. further refinement is still required if the goal is tighter carrier
   accuracy or acceptance-grade flatness statements

Scheduler artifact result from the same batch:

1. `loaded_events = 11`
2. `commit_count = 11`
3. `reinit_count = 1`
4. `error = 0`
5. final snapshot may still show `running = 1` and `done = 0`

That last point is the known status-snapshot caveat: the execution succeeded,
but the sampled status can be stale near completion. Another agent should not
misread that as a failed run when `commit_count == loaded_events` and the host
reported the run as complete.

#### 2b. Scheduler-held SFDR spot set

The same archive includes a scheduler-held SFDR spot set at:

1. `50 MHz`
2. `100 MHz`
3. `150 MHz`
4. `200 MHz`
5. `250 MHz`
6. `300 MHz`
7. `350 MHz`
8. `400 MHz`

Each point is executed as a scheduler-held single-event run with:

1. `dwell_us = 3000000`
2. wide-span spur search on the FSH8

What this proves:

1. the scheduler can be used to hold deterministic steady-state carriers for
   SFDR-style spot measurement
2. the host can package and archive those scheduler-held spot measurements

What it does not yet prove cleanly:

1. that the current SFDR numbers are acceptance-grade
2. that the current broad-span spur-search configuration is the final desired
   SFDR method

Observed limitations in the archived SFDR run:

1. carrier frequency error is still around `1.2 MHz` to `1.9 MHz` for many
   points
2. reported SFDR values are often weak or negative, indicating the current
   wide-span search is still mechanically useful but not yet analytically
   mature

So the right interpretation is:

1. scheduler-held SFDR benchmarking is now mechanically implemented
2. scheduler-held SFDR is not yet the final precision characterization method

### 3. Scope plan archive

[scheduler_scope_plan.json](/C:/Users/fpga_/yr/tmp/no-OS-adi-fork/projects/fmcdac/capture_runs/manual_awg_run/scheduler_scope_plan.json)
exists and is useful as a planning artifact, but it is not scope automation.

It does not prove:

1. that the MSO22 is already automated
2. that timing results have already been captured
3. that a routed timing marker path has already been validated

## What Is Bench-Validated Right Now

These claims are supported by archived runs:

1. legacy dense DDS RF characterization on the FSH8
2. scheduler control-plane bring-up and event execution
3. scheduler-native dense stepped FSH validation for a small multi-event batch
4. scheduler-held single-event RF spot execution for SFDR-style captures
5. host-side batching over the current `64`-event preload limit
6. UARTLite stream correctness smoke for parser, bad CRC, EOF, refill, and
   soft-reset reuse
7. enforced preload/per-step FSH RF coverage smoke over `200-210 MHz`

## What Is Implemented But Not Yet Fully Bench-Validated

These capabilities exist in code or firmware structure, but should not yet be
described as fully validated:

1. dense large-range scheduler-native benchmarking without preload batch
   boundaries
2. scheduler-driven precision SFDR methodology
3. scope-driven latency and pulse-width measurements
4. automated stream regressions for level-sensitive low-watermark re-trigger,
   mode-lock while armed/running, prefetch/hold reset flush, and late-event
   underrun/error recovery

## What Is Still Blocking the Final Intended Suite

### A. Dense stream characterization blocker

The preload `LOADBIN/RUN` path remains the regression oracle. UARTLite
`STREAMHEX` is bench-proven as a correctness path, and coarse per-step stream
RF has passed over `200-210 MHz` in `1 MHz` steps, but it is not yet the dense
`10 kHz` RF characterization or throughput path.

What this means:

1. very dense preload sweeps still incur host reload boundaries
2. preload-batch gap characterization is still relevant
3. stream RF profiles should be expanded from coarse smoke to dense
   characterization only after trace-capable readout or a credible marker-scan
   substitute exists
4. UARTLite stream results should record wall-clock-per-frame and
   wall-clock-per-event for future UART16550/Ethernet comparison

### B. Event-depth blocker

`max_events = 64` blocks single-batch execution of large dense sweeps.

Current consequence:

1. `200-300 MHz` in `10 kHz` steps cannot run as one preload table
2. the scheduler-native suite must still batch

### C. MSO22 automation blocker

There is still no executed MSO22 automation path in the repo.

Missing pieces:

1. instrument control path
2. scope configuration surface
3. capture orchestration
4. automatic measurement reduction
5. archived timing results

### D. Observable timing signal blocker

To use the MSO22 correctly, the system still needs a validated timing
observable. The scope should not be treated as a direct RF-truth instrument for
these scheduler goals.

The first gate is verifying that `marker_commit`, `marker_start`, and
`marker_done` from `awg_timed_ctrl` are routed through the block design and XDC
to physical KCU116 pins. Preferred scope mapping is:

1. CH1: RF envelope detector, mixer IF, or representative analog observable
2. CH2: `marker_commit`
3. optional: stream refill-margin observable such as `IRQ_LOW_WATERMARK`

The canonical scope/register cross-check is `marker_commit` edge count versus
`STREAM_PUSHES` and commit/fire counters during steady stream runs.

## Recommended Interpretation For Another Agent

Another agent should start from these assumptions:

1. the scheduler is real, functional, and already useful for RF benchmarking
2. the current best scheduler RF evidence is
   `fsh_scheduler_preload_perstep_200_210_rf_enforced`
3. the scheduler-native host suite is real and supports preload as the
   regression oracle plus stream for coarse RF smoke
4. stream-mode now has UARTLite correctness bench evidence and coarse RF smoke,
   but it is not yet the validated dense `10 kHz` RF characterization path
5. the MSO22 suite is still a planned implementation area, not a completed one

## Exact Next Engineering Tasks

Priority order:

1. keep archiving stream-specific metrics: bad CRC behavior, EOF/done, reset state,
   low-watermark behavior, `STREAM_STALLS`, `IRQ_UNDERRUN`,
   `IRQ_EMPTY_STALL`, wall-clock-per-frame, and wall-clock-per-event
2. add the missing low-watermark re-trigger, mode-locked, and reset-held-event
   hardware regressions to the automated stream profile
3. expand dense FSH stream coverage beyond the accepted coarse RF smoke only
   after trace-capable readout or a credible marker-scan substitute exists
4. verify marker routing for `marker_commit`, `marker_start`, and `marker_done`
   before implementing MSO22 timing automation
5. build scope-side benchmarks for first-event latency, event-to-event
   switching latency, minimum stable dwell, pulse width, pulse spacing, and
   preload-batch boundary gap
6. tighten the FSH scheduler-held SFDR method so it becomes analytically
   defensible rather than just mechanically runnable

## Current One-Line Status

The project has moved past "can the scheduler run?" and into "how do we turn
it into the final deterministic benchmark engine?" The preload-batched
scheduler-plus-FSH path is already real and bench-validated in limited form;
UARTLite stream bring-up now has correctness bench proof, and coarse stream RF
smoke has passed. Dense `10 kHz` stream RF characterization and MSO22 timing
automation are the remaining major steps.
