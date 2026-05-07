# Scheduler Benchmark Suite

`run_nco_scope_test.py --run-scheduler-benchmark-suite` is the new
scheduler-native host path. It exists to avoid the paused legacy
`Press ENTER to continue...` benchmark flow when the goal is to validate the
uploaded scheduler as the timing engine.

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

2. FSH scheduler SFDR spot set
   - selected carriers are held one-at-a-time by the scheduler
   - the host measures the carrier and strongest spur in the configured search
     band
   - output artifacts:
     - `scheduler_sfdr_spot_set.json`
     - `scheduler_sfdr_spot_set.csv`

3. MSO22 scope plan export
   - the suite now writes `scheduler_scope_plan.json`
   - this is a concrete benchmark plan for timing / pulse-width work
   - it is not yet an instrument-driven execution path in this repo

4. UARTLite stream bring-up profile
   - firmware console commands `STREAMINFO`, `STREAMSTATUS`, `STREAMRESET`,
     and `STREAMHEX <bytes>` are wired to the Phase B frame parser when
     `FMCDAC_AWG_SCHED_STREAM=1`
   - host tooling can pack stream frames, compute CRC32 IEEE, parse ACK/status
     lines, and run a `stream-bringup` correctness profile
   - this profile checks the `STREAM_DEPTH == 511` sentinel, bad-CRC rejection,
     finite EOF completion, and a depth-plus refill run
   - this is code-ready but still needs hardware bench evidence before it is
     treated as a validated stream RF path

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
larger event RAM or the Phase B stream path after the new UARTLite stream
profile is bench-smoked.

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
benchmarking is gated on the stream bring-up profile passing on hardware.

## FSH Benchmarks

The new scheduler-native FSH path can validate:

1. carrier frequency tracking versus programmed step
2. power flatness versus frequency
3. batch-to-batch continuity summaries
4. scheduler-held SFDR spot checks
5. coarse frequency-hop determinism under the scheduler

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
