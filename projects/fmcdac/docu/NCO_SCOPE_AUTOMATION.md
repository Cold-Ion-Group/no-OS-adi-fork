# FSH8 DDS Benchmark Automation

`run_nco_scope_test.py` coordinates the paused FMCDAC firmware diagnostics with
an R&S FSH8 spectrum analyzer.

The current primary workflow is DDS-focused:

1. `DDS-BAND` carrier-level benchmarking
2. `SFDR-TEST` steady-state spur benchmarking
3. optional close-in carrier traces during selected `SFDR-TEST` tones
4. `DYNAMIC-SFDR` retune-burst benchmarking
5. `THROUGHPUT` firmware update-rate baselines
6. `UART-RTT` host latency baselines

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
3. `DYNAMIC-SFDR`
   - `100 MHz <-> 400 MHz`, `1 ms` dwell burst
   - `100 MHz <-> 400 MHz`, `10 ms` dwell burst
4. `THROUGHPUT`
   - firmware-side software update baseline
5. `UART-RTT`
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

### Phase-noise scout traces

During selected SFDR tones, the script can also capture narrow raw traces
centered on the carrier:

1. enable one or more `--phase-noise-span-hz` values
2. optionally pick the SFDR carrier(s) with `--phase-noise-carrier-mhz`
3. optionally override analyzer RBW/VBW/sweep settings for those traces only

This is intended for close-in carrier-skirt capture near `~400 MHz` using the
existing paused firmware flow. It is useful for the next bench campaign, but it
is not an acceptance-grade phase-noise claim by itself.

Current caution:

1. the raw-trace method depends on trace-export SCPI
2. on the current FSH8 `V1.58` firmware, compatibility probing showed the
   trace-export path is not usable:
   - `FORM*` commands are rejected
   - memory-trace commands are rejected
   - `TRAC:DATA? TRACE1` and `TRAC? TRACE1` still fail after the setup is
     reduced to `V1.58`-compatible commands
3. `run_nco_scope_test.py` now fails fast for trace-based requests on this
   legacy firmware instead of waiting for analyzer timeouts

### Marker-only phase-noise offset sweep

The script can also run a marker-only close-in sideband survey that avoids raw
trace export entirely:

1. enable one or more `--phase-noise-offset-hz` values
2. optionally pick the SFDR carrier(s) with `--phase-noise-carrier-mhz`
3. optionally override the narrow sideband span with `--phase-noise-window-hz`
4. the host measures left and right sideband levels at each offset
5. it reports the averaged sideband level in `dBc` and `dBc/Hz`

This is the preferred current-bench method when the trace-export path is not
usable.

### Dynamic retune bursts

The script can also drive a live retune burst and measure during the burst:

1. the firmware toggles between `100 MHz` and `400 MHz` for a fixed-duration run
2. the host measures intended-tone windows near both endpoints
3. the host also searches outside guarded windows for the strongest unintended
   spur

This is the current bench-accessible path for dynamic SFDR / settling work.

Current caution:

1. if `--dynamic-intended-margin-hz` is too narrow, endpoint-adjacent transient
   energy can be classified as the strongest unintended spur
2. the earlier `2 MHz` guard showed that behavior in repeated
   `100 MHz <-> 400 MHz` runs
3. widening only the intended windows is not enough; the unintended spur search
   must exclude the same full intended margin
4. the host now does that correctly
5. use `--dynamic-intended-margin-hz 10000000` for the current confirmation
   rerun if you need a cleaner settling interpretation

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

Dynamic retune burst run:

```powershell
python .\run_nco_scope_test.py `
  --serial-port COM4 `
  --visa-resource "TCPIP::192.168.100.142::INSTR" `
  --visa-backend "@py" `
  --skip-dds-band-test `
  --skip-sfdr-test `
  --skip-throughput-test `
  --skip-uart-rtt `
  --dynamic-trace-mode maxhold `
  --dynamic-sweep-count 1 `
  --dynamic-intended-margin-hz 10000000 `
  --output-dir .\capture_runs\dynamic_sfdr_run
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

Current FSH8 `V1.58` bench status: this option is intentionally blocked by the
host preflight because the analyzer rejects the trace-export path.

Optional close-in carrier traces during SFDR:

```powershell
--phase-noise-span-hz <span_hz>
--phase-noise-carrier-mhz 400
--phase-noise-rbw-hz <rbw_hz>
--phase-noise-vbw-hz <vbw_hz>
--phase-noise-sweep-count <count>
```

Current FSH8 `V1.58` bench status: `--phase-noise-span-hz` is intentionally
blocked by the host preflight. Use the marker-only offset sweep below until the
trace-export path is fixed.

Marker-only phase-noise offset sweep during SFDR:

```powershell
--phase-noise-offset-hz 1000
--phase-noise-offset-hz 10000
--phase-noise-offset-hz 100000
--phase-noise-carrier-mhz 400
--phase-noise-window-hz 100000
--phase-noise-rbw-hz 1000
--phase-noise-vbw-hz 1000
--phase-noise-sweep-count 10
```

Optional dynamic retune burst tuning:

```powershell
--dynamic-rbw-hz <rbw_hz>
--dynamic-vbw-hz <vbw_hz>
--dynamic-sweep-count <count>
--dynamic-trace-mode maxhold
--dynamic-detector positive
--dynamic-intended-margin-hz 2000000
--skip-dynamic-sfdr-test
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
6. `phase_noise_results.csv` when close-in traces are requested
7. `phase_noise_offset_results.csv` when marker-only offset sweeps are requested
8. `dynamic_sfdr_results.csv` when retune bursts are requested
9. `throughput.json`
10. `uart_rtt.json`
11. per-step CSV/JSON files

For SFDR steps, the per-step CSV contains the carrier, left-spur, right-spur,
and worst-spur marker summary.

## Bench Setup Suggestions

1. Use the FSH8 for DDS-band and SFDR work above `200 MHz`.
2. Use direct coax and `50 ohm` input.
3. Start with the conservative analyzer settings listed above.
4. Keep attenuation and reference level documented for each run.

## TODOs

1. Get the exact R&S-supported trace-export path for FSH8 firmware `V1.58`, or
   upgrade the analyzer firmware to a version that supports the documented
   `TRACe<n>[:DATA]?` flow.
2. After firmware or syntax changes, rerun `fsh_trace_probe.py` before enabling
   trace capture in the main benchmark flow.
3. If live trace export remains unavailable, evaluate instrument-side
   `MMEM:DATA?` file export as the next fallback.
4. Keep marker-only `--phase-noise-offset-hz` as the current working close-in
   method until a dense trace path is proven.
5. Treat `summary.json` and `sfdr_results.csv` as the primary outputs for
   comparing runs.
