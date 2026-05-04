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

## Dependencies

- Python with pyserial installed for UART access.
- The analyzer stack (`pyvisa`, `pyvisa-py`) is required for measured AWG runs
  and for `run_nco_scope_test.py`.
