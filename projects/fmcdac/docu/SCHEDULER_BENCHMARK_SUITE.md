# Scheduler Benchmark Suite

`run_nco_scope_test.py --run-scheduler-benchmark-suite` is the new
scheduler-native host path. It exists to avoid the paused legacy
`Press ENTER to continue...` benchmark flow when the goal is to validate the
uploaded scheduler as the timing engine.

## Current Bench Status

Status as of `2026-05-20`:

1. Legacy preload scheduler execution is bench-validated on the active KCU116
   image at `base=0x44AA0000`.
2. The active scheduler clock is `tick_hz=245760000`, matching the Phase A
   scheduler clock domain. Do not use older `100000000` tick assumptions for
   new event timestamps.
3. UARTLite stream bring-up has passed as a correctness smoke:
   - `IP_ID == 0x41574753`
   - `IP_VERSION == 0x00010000`
   - `STREAM_DEPTH == 511`
   - bad CRC rejects without changing accepted counters
   - finite EOF stream completes with `eof_seen=1`, `done=1`, `error=0`
   - depth-plus refill run reached `STREAM_PUSHES=32`, `commit=32`
4. FSH8 full-sweep `maxhold` is not valid RF evidence on the current
   `Rohde&Schwarz FSH8 V1.58` bench. Trace export fails, marker fallback reads
   floor-level flatlines, and the host correctly marks those bins
   `marker_flatline_untrusted`.
5. The current RF coverage path is scheduler preload plus per-step FSH capture.
   Latest accepted 11-tone smoke over `200-210 MHz @ 1 MHz`:
   - `loaded=11`, `commit=11`, `done=1`, `error=0`
   - `rf_quality.passed=true`
   - max absolute frequency error about `495 kHz`
   - flatness about `2.98 dB`
   - max peak-vs-marker delta about `9.85 dB`

## Current Scope

Implemented now:

1. FSH scheduler dense sweep
   - host splits large sweeps into batches when `event_count > max_events`
   - each batch is uploaded through the AWG scheduler console
   - each programmed tone is measured in its own dwell window on the FSH
   - output artifacts:
     - `scheduler_dense_sweep.json`
     - `scheduler_dense_sweep_plot.csv`
     - `scheduler_dense_sweep_plot.svg`

2. FSH scheduler full-sweep max-hold
   - optional `--scheduler-fsh-capture-mode maxhold` captures the whole sweep
     in one analyzer span instead of measuring every dwell window separately
   - readout is hybrid by default: full trace export when supported, marker
     frequency readback on legacy FSH8 firmware
   - marker fallback rejects floor-level flatline readout by default because
     FSH8 V1.58 can return the same marker floor for every forced marker
     position; override only with `--scheduler-fsh-allow-marker-flatline`
     after independently confirming the RF chain
   - optional `--scheduler-fsh-calibrate-capture quick|exhaustive` searches
     RBW/VBW/dwell/repeat candidates before the real run
   - output artifacts:
     - `scheduler_full_sweep_maxhold.json`
     - `scheduler_full_sweep_maxhold_bins.csv`
     - `scheduler_full_sweep_maxhold_trace.csv` when trace export works
     - `scheduler_full_sweep_maxhold_plot.svg`

3. FSH scheduler SFDR spot set
   - selected carriers are held one-at-a-time by the scheduler
   - the host measures the carrier and strongest spur in the configured search
     band
   - output artifacts:
     - `scheduler_sfdr_spot_set.json`
     - `scheduler_sfdr_spot_set.csv`

4. MSO22 scope plan export
   - the suite now writes `scheduler_scope_plan.json`
   - this is a concrete benchmark plan for timing / pulse-width work
   - it is not yet an instrument-driven execution path in this repo

5. UARTLite stream bring-up profile
   - firmware console commands `STREAMINFO`, `STREAMSTATUS`, `STREAMRESET`,
     and `STREAMHEX <bytes>` are wired to the Phase B frame parser when
     `FMCDAC_AWG_SCHED_STREAM=1`
   - host tooling can pack stream frames, compute CRC32 IEEE, parse ACK/status
     lines, and run a `stream-bringup` correctness profile
   - this profile checks the `STREAM_DEPTH == 511` sentinel, bad-CRC rejection,
     finite EOF completion, and a depth-plus refill run
   - this is bench-validated as a correctness path, and coarse stream RF smoke
     has passed over `200-210 MHz` in `1 MHz` steps; dense `10 kHz` RF
     characterization remains blocked on trace-capable readout or a credible
     marker-scan substitute

Also emitted for every scheduler-native suite run:

- `scheduler_benchmark_catalog.json`
- `scheduler_benchmark_suite.json`

## Important Limitations

The current bench image reports `max_events=64`.

That means a dense sweep such as `200 MHz` to `300 MHz` in `10 kHz` steps does
not fit in one autonomous hardware table. The current host solves that by
batching:

1. upload one scheduler batch
2. run and measure it
3. upload the next batch

So this path is already useful for dense validation, but it is not yet the same
as a single uninterrupted `10001`-event hardware schedule. That would require a
larger event RAM or the Phase B stream path. The stream path is now smoked for
correctness and coarse RF execution, but UARTLite is still too slow for
throughput stress and the current FSH8 marker path is not enough for dense
`10 kHz` spectral characterization.

Legacy preload comparisons are only valid for schedules that fit the legacy
event BRAM. Treat `EVENT_MEM_ADDR_WIDTH=8` as the architectural ceiling:

1. `<=256` events: direct preload-vs-stream comparison is meaningful
2. `>256` events: stream-only, no full preload baseline exists
3. dense `10 kHz` sweeps should be compared against historical dense DDS runs
   and smaller preload subsets, not against a nonexistent full preload run

The first stream transport is UARTLite console ASCII hex at `115200` baud.
This is a correctness and observability path, not a throughput path:

1. raw UART payload is about `11.5 KB/s`
2. each `32` byte event becomes `64` hex characters before command, CRC, ACK,
   and newline overhead
3. realistic sustained throughput is about `100-150 events/s`
4. `10,000` events should be expected to take about `67-100 s` to stream
5. `100,000` events should be expected to take about `11-17 min` to stream and
   should be treated as a leak/correctness soak, not transport stress
6. every stream manifest records wall-clock-per-frame and wall-clock-per-event
   so UART16550/Ethernet can be compared later

## Stream-Mode Firmware Status

The refreshed Phase A HDL exposes the stream FIFO ABI:

1. `STREAM_CTRL`
2. `OCCUPANCY`
3. `FREE_SPACE`
4. `LOW_WMARK`
5. `STREAM_DEPTH`
6. `STREAM_PUSHES`
7. `STREAM_STALLS`

Phase B firmware now implements a DDR-staged stream refill path behind
`FMCDAC_AWG_SCHED_STREAM`. The implementation is build-verified in stream-off
and stream-on configurations, and includes a transport-neutral frame parser in
`src/app/awg_stream_proto.{c,h}`.

The scheduler benchmark suite now has a stream bring-up profile:

```powershell
make clean
make scheduler-stream
make run
```

```powershell
python .\run_nco_scope_test.py `
  --serial-port COM4 `
  --run-scheduler-benchmark-suite `
  --scheduler-suite-profile stream-bringup `
  --scheduler-transport stream `
  --skip-make-run `
  --awg-sched-baseaddr 0x44AA0000 `
  --output-dir .\capture_runs\scheduler_stream_bringup
```

FSH dense and SFDR profiles still use legacy preload execution today. Stream RF
benchmarking through the per-step path remains gated on later work. The
full-sweep max-hold path can run `preload`, `stream`, or `compare` transports;
for stream, keep UARTLite bandwidth in mind and use large timestamp margins.

## FSH Benchmarks

The new scheduler-native FSH path can validate:

1. carrier frequency tracking versus programmed step
2. power flatness versus frequency
3. batch-to-batch continuity summaries
4. scheduler-held SFDR spot checks
5. coarse frequency-hop determinism under the scheduler

The repeat command for the current accepted preload/per-step RF smoke is:

```powershell
python .\run_nco_scope_test.py `
  --serial-port COM4 `
  --baudrate 115200 `
  --visa-resource "TCPIP::192.168.100.142::INSTR" `
  --visa-backend "@py" `
  --skip-make-run `
  --run-scheduler-benchmark-suite `
  --scheduler-suite-profile dense `
  --scheduler-transport preload `
  --scheduler-fsh-capture-mode per-step `
  --awg-sched-baseaddr 0x44AA0000 `
  --awg-sweep-start-hz 200000000 `
  --awg-sweep-stop-hz 210000000 `
  --awg-sweep-step-hz 1000000 `
  --awg-sweep-dwell-us 10000000 `
  --awg-sweep-scale-u 1200000 `
  --rbw-hz 10000 `
  --vbw-hz 10000 `
  --sweep-count 10 `
  --reference-level-dbm -60 `
  --preamplifier on `
  --rf-power-correction-db 0 `
  --rf-power-calibration-label "raw-fsh8-direct" `
  --scheduler-dense-rf-max-freq-error-hz 500000 `
  --scheduler-dense-rf-max-peak-marker-delta-db 14 `
  --scheduler-dense-rf-min-power-dbm -100 `
  --scheduler-dense-rf-max-flatness-db 6 `
  --scheduler-dense-rf-enforce `
  --scheduler-keep-console-open `
  --output-dir .\capture_runs\fsh_scheduler_preload_perstep_200_210_rf_enforced
```

The RF-quality thresholds are intentionally bench-smoke thresholds, not
precision metrology limits. The `peak_marker_delta` threshold is relaxed to
`14 dB` because the FSH8 V1.58 marker/peak behavior in narrow per-step captures
still shows local-peak ambiguity. Keep stricter thresholds for single-tone
debug runs.

Power readings are raw analyzer dBm unless an RF path correction is supplied.
Use `--rf-power-correction-db <dB>` for a fixed cable/attenuator correction and
`--rf-power-calibration-csv <csv>` for a frequency-dependent correction table.
The table format is `frequency_hz,correction_db`; the host linearly
interpolates between points and writes both raw and corrected power fields into
the JSON/CSV artifacts. The dense RF quality gate uses corrected power when it
is present.

For the first stream-backed RF smoke, keep the same analyzer settings and move
only the scheduler transport:

```powershell
python .\run_nco_scope_test.py `
  --serial-port COM4 `
  --baudrate 115200 `
  --visa-resource "TCPIP::192.168.100.142::INSTR" `
  --visa-backend "@py" `
  --skip-make-run `
  --run-scheduler-benchmark-suite `
  --scheduler-suite-profile dense `
  --scheduler-transport stream `
  --scheduler-fsh-capture-mode per-step `
  --awg-sched-baseaddr 0x44AA0000 `
  --scheduler-stream-depth-sentinel 511 `
  --scheduler-stream-frame-events 16 `
  --scheduler-stream-hex-line-chars 8 `
  --scheduler-stream-hex-chunk-delay-s 0.02 `
  --awg-sweep-start-hz 200000000 `
  --awg-sweep-stop-hz 210000000 `
  --awg-sweep-step-hz 1000000 `
  --awg-sweep-dwell-us 10000000 `
  --awg-sweep-scale-u 1200000 `
  --rbw-hz 10000 `
  --vbw-hz 10000 `
  --sweep-count 10 `
  --reference-level-dbm -60 `
  --preamplifier on `
  --scheduler-dense-rf-max-freq-error-hz 500000 `
  --scheduler-dense-rf-max-peak-marker-delta-db -1 `
  --scheduler-dense-rf-min-power-dbm -100 `
  --scheduler-dense-rf-max-flatness-db 6 `
  --scheduler-dense-rf-enforce `
  --scheduler-keep-console-open `
  --output-dir .\capture_runs\fsh_scheduler_stream_perstep_200_210_rf_enforced
```

This path uploads the finite EOF-marked stream first, anchors per-step capture
from the first stream ACK, then waits for `done/eof_seen`. UARTLite upload time
is still correctness-only; do not use this result as a transport-throughput
claim.

For broad RF coverage without per-step analyzer timing, use full-sweep
max-hold:

```powershell
python .\run_nco_scope_test.py `
  --serial-port COM4 `
  --baudrate 115200 `
  --visa-resource "TCPIP::192.168.100.142::INSTR" `
  --visa-backend "@py" `
  --skip-make-run `
  --run-scheduler-benchmark-suite `
  --scheduler-suite-profile fsh `
  --scheduler-transport preload `
  --scheduler-fsh-capture-mode maxhold `
  --scheduler-fsh-readout hybrid `
  --scheduler-fsh-calibrate-capture exhaustive `
  --awg-sched-baseaddr 0x44AA0000 `
  --awg-sweep-start-hz 200000000 `
  --awg-sweep-stop-hz 210000000 `
  --awg-sweep-step-hz 1000000 `
  --output-dir .\capture_runs\fsh_scheduler_preload_maxhold
```

`maxhold` validates tone coverage and flatness. It does not prove exact
event timing or event-to-event boundary behavior; use MSO22/register-counter
tests for that.

On FSH8 V1.58, trace export usually fails and `hybrid` falls back to marker
readout. A run that reports identical low-level power for every expected bin is
not valid RF evidence; the host marks those bins missing with
`marker_flatline_untrusted`. Tune the guard with
`--scheduler-fsh-marker-flatline-floor-dbm` and
`--scheduler-fsh-marker-flatline-epsilon-db`, or disable it only for a known
strong flat response with `--scheduler-fsh-allow-marker-flatline`.

Current calibration status: the suite can record and apply RF path correction
metadata, but the bench is not yet a fully calibrated absolute-power system.
Until a documented RF path loss/gain table and analyzer level check are
captured, treat FSH8 scheduler results as relative/coarse RF evidence.

## MSO22 Benchmarks Planned

The emitted scope plan covers:

1. epoch-to-first-event latency
2. event-to-event switch latency
3. minimum stable dwell / minimum observable pulse width around
   `MIN_SPACING_TICKS = 8` at `sched_clk = 245.76 MHz`
4. pulse-width and spacing accuracy versus programmed tick values
5. batch-boundary dead time caused by host-side batching
6. reinit-versus-no-reinit observability

Before scope execution, verify that `marker_commit`, `marker_start`, and
`marker_done` from `awg_timed_ctrl` are routed through the block design and
constraints to physical KCU116 pins. Preferred scope mapping is CH1 as RF
envelope/detector and CH2 as `marker_commit`; a routed
`IRQ_LOW_WATERMARK`-style signal is a useful stream refill-margin observable.

## Example

```powershell
python .\run_nco_scope_test.py `
  --serial-port COM4 `
  --visa-resource "TCPIP::192.168.100.142::INSTR" `
  --visa-backend "@py" `
  --xilinx-settings "C:\Xilinx\Vivado\2021.2\settings64.bat" `
  --xilinx-settings "C:\Xilinx\Vitis_HLS\2021.2\settings64.bat" `
  --xilinx-settings "C:\Xilinx\Vitis\2021.2\settings64.bat" `
  --run-scheduler-benchmark-suite `
  --scheduler-suite-profile all `
  --awg-sched-baseaddr 0x44AA0000 `
  --awg-sweep-start-hz 200000000 `
  --awg-sweep-stop-hz 300000000 `
  --awg-sweep-step-hz 10000 `
  --awg-sweep-dwell-us 2000000 `
  --output-dir .\capture_runs\scheduler_benchmark_suite
```
