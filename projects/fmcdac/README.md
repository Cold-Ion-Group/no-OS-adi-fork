# FMCDAC Quick Notes

## AWG scheduler sweep test

The AWG sweep test uses the dedicated AWG scheduler UART console. The host
builds an `awg_event_v1_t[]` payload, uploads it as ASCII hex over UART,
starts the hardware scheduler, and parses the `[SCHED-ARTIFACT]` results.

Two operating modes now exist:

1. control-plane validation only
   - no analyzer required
   - proves upload, commit, epoch reload, and event execution
2. measured AWG validation
   - requires an FSH8 VISA resource
   - measures each programmed tone during its dwell window and writes
     per-step artifacts plus `awg_scheduler_run.json`

Example (PowerShell):

```powershell
python .\awg_sweep_test.py `
  --serial-port COM4 `
  --visa-resource "TCPIP::192.168.100.142::INSTR" `
  --visa-backend "@py" `
  --xilinx-settings "C:\Xilinx\Vivado\2021.2\settings64.bat" `
  --xilinx-settings "C:\Xilinx\Vitis_HLS\2021.2\settings64.bat" `
  --xilinx-settings "C:\Xilinx\Vitis\2021.2\settings64.bat" `
  --awg-sched-baseaddr 0x44AA0000 `
  --awg-sweep-start-hz 200000000 `
  --awg-sweep-stop-hz 210000000 `
  --awg-sweep-step-hz 1000000 `
  --output-dir .\capture_runs\awg_sched_measured
```

Notes:
- The AWG scheduler base address must match the bitstream.
- The current KCU116 image reports `max_events=64`. Dense one-shot sweeps such
  as `200-300 MHz` in `10 kHz` steps do not fit in the current event RAM.
- The current measured host path validates one loaded batch at a time. Very
  dense sweeps therefore require host-side batching or a larger HDL event RAM.
- The refreshed Phase A HDL also exposes stream-mode scheduler registers and a
  stream FIFO. Phase B firmware support exists behind
  `FMCDAC_AWG_SCHED_STREAM`. UARTLite `STREAMHEX` has passed correctness
  smoke testing, and scheduler dense per-step FSH has now passed a coarse
  stream RF smoke over `200-210 MHz` in `1 MHz` steps. Dense `10 kHz`
  granularity RF characterization and throughput stress remain deferred to
  trace-capable readout and/or a higher-rate transport.
- The current payload contract supports `DDS_PHASE_DW=32`; this is no longer a
  16-bit FTW-only path.
- When an analyzer is attached, the host selects a longer default dwell and a
  narrow expected-tone measurement window so the FSH8 can finish each
  measurement inside the programmed step.

## Full integration wrapper

`run_nco_scope_test.py --run-full-integration` is a two-pass wrapper:

1. pass 1 runs the uploaded AWG scheduler sweep with optional analyzer
   validation
2. pass 2 reruns the existing legacy prompt-driven DDS-band / SFDR / dynamic /
   throughput / UART-RTT suite

This is intentionally not yet a scheduler-native replacement for the whole
benchmark engine. The second pass still uses the paused firmware prompt flow.

## Scheduler-native benchmark suite

`run_nco_scope_test.py --run-scheduler-benchmark-suite` is the new
scheduler-native host path. It does not go through the legacy paused benchmark
prompts. Instead it keeps the UART session in the AWG scheduler console and
runs scheduler-driven batches directly.

Current first-phase coverage:

- FSH dense stepped sweep with host-side batching over the current `64`-event
  limit
- enforced dense RF quality summaries for scheduler-driven per-step captures
- FSH scheduler-held SFDR spot set
- UARTLite stream correctness profile (`stream-bringup`)
- full-sweep max-hold artifact generation with guard rails for FSH8 V1.58
  marker flatline failures
- RF power calibration provenance in artifacts via fixed correction and
  optional frequency-table correction fields
- emitted `scheduler_scope_plan.json` for MSO22 timing / pulse-width work

Current scheduler transport status:

- legacy `LOADBIN/RUN` UART console remains the active RF bench path
- stream-mode firmware and parser are wired to a conservative UARTLite
  ASCII-hex `STREAMHEX` smoke transport
- scheduler dense per-step FSH can now run through stream transport for
  finite EOF-marked RF smoke runs
- UARTLite stream is correctness/observability only; throughput and dense RF
  stress are deferred to UART16550, Ethernet, or another higher-rate transport

Current accepted scheduler RF smoke command:

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

The RF power correction flags are optional. Leave
`--rf-power-correction-db 0` for raw FSH8 readings, or pass a positive dB
value for known cable/attenuator loss between the DUT and analyzer. For
frequency-dependent correction, add `--rf-power-calibration-csv path.csv`;
the CSV format is `frequency_hz,correction_db`.

See [docu/SCHEDULER_BENCHMARK_SUITE.md](./docu/SCHEDULER_BENCHMARK_SUITE.md).
For a current bench handoff covering the latest archived runs and the intended
end-state validation plan, see
[docu/SCHEDULER_HANDOFF_STATUS.md](./docu/SCHEDULER_HANDOFF_STATUS.md).

## Dependencies

- Python with pyserial installed for UART access.
- The analyzer stack (`pyvisa`, `pyvisa-py`) is required for measured AWG runs
  and for `run_nco_scope_test.py`.
