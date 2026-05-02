# FMCDAC Quick Notes

## AWG scheduler sweep test

The AWG sweep test drives the firmware prompt flow over UART and runs a timed
DDS sweep using the AWG scheduler IP. It does not require the analyzer.

Example (PowerShell):

```powershell
python awg_sweep_test.py \
  --serial-port COM4 \
  --xilinx-settings "C:\Xilinx\Vivado\2024.2\settings64.bat" \
  --awg-sched-baseaddr 0x44AA0000 \
  --awg-sweep-start-hz 200000000 \
  --awg-sweep-stop-hz 210000000 \
  --awg-sweep-step-hz 1000000 \
  --awg-sweep-dwell-us 1000
```

Notes:
- The AWG scheduler base address must match the bitstream.
- The default firmware event depth is 64; keep the sweep size small or pass
  --awg-sched-max-events to override.
- The sweep uses 16-bit DDS FTW and will skip if DDS_PHASE_DW > 16.

## Dependencies

- Python with pyserial installed for UART access.
- The analyzer stack (pyvisa) is only required for run_nco_scope_test.py.
