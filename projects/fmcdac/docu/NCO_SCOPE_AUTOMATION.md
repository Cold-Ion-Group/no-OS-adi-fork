# FSH8 DDS Benchmark Automation

`run_nco_scope_test.py` coordinates the paused FMCDAC firmware diagnostics with
an R&S FSH8 spectrum analyzer.

The current primary workflow is DDS-focused:

1. `DDS-BAND` carrier-level benchmarking
2. `SFDR-TEST` steady-state spur benchmarking
3. `THROUGHPUT` firmware update-rate baselines
4. `UART-RTT` host latency baselines

`NCO-TEST` still exists in the firmware, but it is now optional and skipped by
default.

## What The Script Does

The script waits for the paused prompts in [fmcdac.c](/C:/Users/fpga_/yr/tmp/no-OS-adi-fork/projects/fmcdac/src/app/fmcdac.c) and advances them automatically.

Primary steps:

1. `DDS-BAND`
   - `10/100/200 MHz`
   - `230-330 MHz` in `10 MHz` steps
2. `SFDR-TEST`
   - `50-400 MHz` in `50 MHz` steps
3. `THROUGHPUT`
   - firmware-side software update baseline
4. `UART-RTT`
   - host-side ping/pong latency baseline

Optional step:

1. `NCO-TEST`
   - only if `--run-nco-test` is given

## Measurement Policy

### DDS-band

For DDS-band checkpoints the script does a narrow analyzer measurement and
captures:

1. peak power in dBm
2. peak frequency
3. optional raw trace data if `--capture-trace` is enabled

### SFDR

For SFDR checkpoints the script now uses segmented marker-based sweeps:

1. sweep a narrow span around the expected carrier
2. exclude a guard band around the actual carrier
3. sweep the left and right spur regions separately
4. compute `SFDR = carrier - worst spur`

This is required because:

1. simple marker-window searches on the FSH8 could reacquire the carrier as the
   spur
2. full `TRAC:DATA? TRACE1` wideband readback was timing out on this setup

## Build/Run Behavior

By default the script runs:

```powershell
make run
```

from `projects/fmcdac`.

It does not rotate `.Xil`, does not rotate `build`, and does not create
temporary wrapper `.cmd` files.

If your shell does not already have the Xilinx environment, pass the settings
batch files explicitly with repeated `--xilinx-settings` arguments.

## Example Run

Normal DDS/SFDR benchmark run:

```powershell
python .\run_nco_scope_test.py `
  --serial-port COM4 `
  --visa-resource "TCPIP::192.168.100.142::INSTR" `
  --visa-backend "@py" `
  --xilinx-settings "C:\Xilinx\Vivado\2021.2\settings64.bat" `
  --xilinx-settings "C:\Xilinx\Vitis_HLS\2021.2\settings64.bat" `
  --xilinx-settings "C:\Xilinx\Vitis\2021.2\settings64.bat" `
  --analyzer-timeout 30
```

If you want to include the legacy NCO diagnostic too:

```powershell
python .\run_nco_scope_test.py `
  --serial-port COM4 `
  --visa-resource "TCPIP::192.168.100.142::INSTR" `
  --visa-backend "@py" `
  --run-nco-test
```

If the board is already running and waiting at the first paused prompt:

```powershell
python .\run_nco_scope_test.py `
  --serial-port COM4 `
  --visa-resource "TCPIP::192.168.100.142::INSTR" `
  --visa-backend "@py" `
  --skip-make-run `
  --resume-at-nco
```

## Useful Options

General analyzer setup:

```powershell
--reference-level-dbm 0
--display-range-db 80
--rbw-hz 100000
--vbw-hz 100000
--sweep-count 3
--trace-mode average
--detector positive
--attenuation-auto on
--preamplifier off
--input-impedance 50
```

SFDR search tuning:

```powershell
--sfdr-start-hz 5000000
--sfdr-stop-hz 1000000000
--sfdr-guard-hz 2000000
```

Skip selected firmware prompts:

```powershell
--skip-dds-band-test
--skip-sfdr-test
--skip-throughput-test
--skip-uart-rtt
```

Optional raw narrow-span traces for DDS-band/NCO:

```powershell
--capture-trace
```

## Outputs

If `--output-dir` is not supplied, artifacts are written to:

```text
projects/fmcdac/capture_runs/<UTC timestamp>/
```

Typical outputs:

1. `uart.log`
2. `summary.json`
3. `dds_band_plot.csv`
4. `dds_band_plot.svg`
5. `sfdr_results.csv`
6. `throughput.json`
7. `uart_rtt.json`
8. per-step CSV/JSON files

For SFDR steps, the per-step CSV contains the carrier, left-spur, right-spur,
and worst-spur marker summary.

## Bench Setup Suggestions

1. Use the FSH8 for DDS-band and SFDR work above `200 MHz`.
2. Use direct coax and `50 ohm` input.
3. Start with the conservative analyzer settings listed above.
4. Keep attenuation and reference level documented for each run.
5. Treat `summary.json` and `sfdr_results.csv` as the primary outputs for
   comparing runs.
