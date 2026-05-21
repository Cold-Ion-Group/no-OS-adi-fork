# FMCDAC Analog and Dynamic Bench Report

Date: 2026-05-07

## Artifact Paths

Dense analog map plus sparse SFDR/phase-noise:

`C:\Users\fpga_\yr\tmp\no-OS-adi-fork\projects\fmcdac\capture_runs\matrix12_dense_dds_sparse_sfdr_phase`

Dynamic retune run:

`C:\Users\fpga_\yr\tmp\no-OS-adi-fork\projects\fmcdac\capture_runs\dynamic_only_200_300_225_275`

Main host script:

`C:\Users\fpga_\yr\tmp\no-OS-adi-fork\projects\fmcdac\run_nco_scope_test.py`

Firmware entry point:

`C:\Users\fpga_\yr\tmp\no-OS-adi-fork\projects\fmcdac\src\app\fmcdac.c`

## Bench Setup

Analyzer:

`Rohde&Schwarz,FSH8,106469/028,V1.58`

Control links:

- UART: `COM4 @ 115200`
- VISA resource: `TCPIP::192.168.100.142::INSTR`
- VISA backend: `@py`
- Xilinx toolchain settings:
  - `C:\Xilinx\Vivado\2021.2\settings64.bat`
  - `C:\Xilinx\Vitis_HLS\2021.2\settings64.bat`
  - `C:\Xilinx\Vitis\2021.2\settings64.bat`

Firmware banner for the dense analog run:

`control-plane-work-48be4ef29-modified  May 5 2026 22:42:19`

Common link/DAC configuration from UART:

- JESD subclass: `1`
- Lane rate: `9830400 kbps`
- Reference clock: `122880 kHz`
- DAC clock reported in banner: `983040 kHz`
- DAC mode: `M=2 L=4 F=1 S=1 HD=1 N=16 N'=16 K=32`
- Interpolation mode during DDS/dynamic tests: `0x01`
- NCO disabled for DDS-band and dynamic tests

## Dense Analog Map Capture

Run path:

`capture_runs\matrix12_dense_dds_sparse_sfdr_phase`

Purpose:

Measure amplitude versus DDS frequency across the experiment band.

Firmware behavior:

- Host requested custom DDS-band sweep.
- Firmware printed `Capturing 10001 custom points from 200000000 Hz to 300000000 Hz in 10000 Hz steps`.
- For each point, firmware programmed both DAC channels to the same DDS frequency with scale `999000`.
- Host advanced each prompt and captured analyzer marker power.

Analyzer settings from `summary.json`:

- RBW: `100 kHz`
- VBW: `100 kHz`
- Sweep count: `3`
- Trace mode: `average`
- Detector: `positive`
- Reference level: `0 dBm`
- Display range: `80 dB`
- Trace export disabled because the FSH8 V1.58 SCPI path is limited.

Dense DDS-band result file:

`dds_band_plot.csv`

Plot file:

`dds_band_plot.svg`

Dense DDS-band summary:

- Points: `10001`
- Frequency range: `200.000000 MHz` to `300.000000 MHz`
- Step: `10 kHz`
- Measured level range: `-104.73 dBm` to `-82.05 dBm`
- Mean measured level: `-91.99 dBm`
- Delta relative to first point: `-11.09 dB` to `+11.59 dB`

Interpretation:

The dense run is useful as a first amplitude map of the 200-300 MHz experiment band. It should be presented as a bench-level relative amplitude map, not as a final calibrated output-power specification, because the measured absolute powers are low and the RF path calibration is not yet documented.

## Sparse SFDR Capture

Run path:

`capture_runs\matrix12_dense_dds_sparse_sfdr_phase`

Purpose:

Measure spectral quality at representative carriers instead of running a dense SFDR sweep.

Requested carriers:

- `200 MHz`
- `225 MHz`
- `250 MHz`
- `275 MHz`
- `300 MHz`

Result file:

`sfdr_results.csv`

Recorded results:

| Carrier MHz | Measured carrier MHz | Error MHz | Carrier dBm | Worst spur MHz | Worst spur dBm | SFDR dB |
|---:|---:|---:|---:|---:|---:|---:|
| 200 | 200.736508 | +0.736508 | -85.34 | 6.845110 | -82.70 | -2.64 |
| 225 | 224.003175 | -0.996825 | -87.69 | 8.100045 | -83.25 | -4.44 |
| 250 | 248.177778 | -1.822222 | -85.46 | 54.384021 | -84.36 | -1.10 |
| 275 | 275.825397 | +0.825397 | -86.61 | 54.498009 | -83.44 | -3.17 |
| 300 | 299.974603 | -0.025397 | -85.93 | 40.808007 | -83.00 | -2.93 |

Interpretation:

These SFDR rows are not publication-quality as-is. The analyzer selected low-frequency peaks as the strongest spurs, and several carrier detections are offset by about `0.7-1.8 MHz`. This indicates that the current sparse SFDR metric is still too dependent on wide peak search behavior. The result is useful for debugging the measurement method, but the SFDR number should not be used as a claim about the CORDIC or DDS spectral purity.

## Phase-Noise Offset Capture

Run path:

`capture_runs\matrix12_dense_dds_sparse_sfdr_phase`

Purpose:

Measure marker-only sideband power at representative carriers and offsets.

Requested carriers:

- `200 MHz`
- `250 MHz`
- `300 MHz`

Requested offsets:

- `10 kHz`
- `100 kHz`

Result file:

`phase_noise_offset_results.csv`

Recorded results:

| Carrier MHz | Offset Hz | Measured carrier MHz | Carrier error MHz | Avg sideband dBc/Hz |
|---:|---:|---:|---:|---:|
| 200 | 10000 | 200.736508 | +0.736508 | -39.62 |
| 200 | 100000 | 200.736508 | +0.736508 | -39.58 |
| 250 | 10000 | 248.177778 | -1.822222 | -40.94 |
| 250 | 100000 | 248.177778 | -1.822222 | -41.16 |
| 300 | 10000 | 299.974603 | -0.025397 | -42.99 |
| 300 | 100000 | 299.974603 | -0.025397 | -42.89 |

Interpretation:

The 300 MHz rows are the most believable because the carrier frequency was found within `25 kHz` of the command. The 200 MHz and 250 MHz rows inherit the same carrier-selection error seen in the SFDR path, so they should be treated as method-limited until carrier acquisition is marker-locked or otherwise narrowed.

## Dynamic Retune Capture

Run path:

`capture_runs\dynamic_only_200_300_225_275`

Purpose:

Exercise parameterized dynamic retune bursts relevant to the intended 200-300 MHz operating band.

Firmware behavior:

- Host generated firmware compile defines for 4 custom dynamic cases.
- Firmware confirmed `Running 4 custom dynamic retune burst(s)`.
- DDS scale fixed to `700000`.
- NCO disabled.
- Firmware toggled the programmed DDS frequency during each burst.

Requested cases:

| Case | Start MHz | Stop MHz | Dwell ms | Transitions | Nominal active time |
|---:|---:|---:|---:|---:|---:|
| 1 | 200 | 300 | 1 | 12000 | 12 s |
| 2 | 200 | 300 | 10 | 1200 | 12 s |
| 3 | 225 | 275 | 1 | 12000 | 12 s |
| 4 | 225 | 275 | 10 | 1200 | 12 s |

Firmware elapsed times from UART:

| Case | Dwell ms | Firmware elapsed us | Observation |
|---:|---:|---:|---|
| 1 | 1 | 1817724 | Much shorter than nominal 12 s |
| 2 | 10 | 15262347 | Longer than nominal 12 s |
| 3 | 1 | 1700659 | Much shorter than nominal 12 s |
| 4 | 10 | 15250582 | Longer than nominal 12 s |

Dynamic aggregate result file:

`dynamic_sfdr_results.csv`

Recorded analyzer results from the latest completed run:

| Case | Dwell ms | Elapsed us | us/transition | Intended 1 | Intended 1 dBm | Intended 2 | Intended 2 dBm | Strongest spur MHz | Spur dBm | Margin dB |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| dynamic_toggle_200_300_1ms | 1 | 1817724 | 151.477 | 200.000000 MHz | -88.22 | 300.000000 MHz | -89.06 | 7.055556 | -80.90 | -7.32 |
| dynamic_toggle_200_300_10ms | 10 | 15262345 | 12718.621 | 200.000000 MHz | -103.38 | 300.000000 MHz | -92.64 | 18.214286 | -80.56 | -12.08 |
| dynamic_toggle_225_275_1ms | 1 | 1700661 | 141.722 | 225.000000 MHz | -89.88 | 275.000000 MHz | -97.99 | 48.000000 | -80.48 | -9.40 |
| dynamic_toggle_225_275_10ms | 10 | 15250581 | 12708.818 | 225.000000 MHz | -90.66 | 275.000000 MHz | -93.33 | 9.666667 | -79.87 | -10.79 |

Interpretation:

The dynamic run is a valid integration pass: the firmware, host prompts, and parameterized dynamic cases all executed. The latest result also confirms that the host now measures the intended carriers at the exact requested marker frequencies instead of selecting a broad-window maximum.

The analog dynamic result is still not publication-quality. In all four rows, the strongest unintended peak is stronger than the intended carrier markers, producing negative dynamic spur margins from about `-7.3 dB` to `-12.1 dB`. The unintended peaks are also low-frequency artifacts rather than close-in retune products, which points to RF path/analyzer configuration or output coupling issues that must be isolated before using these numbers as a spectral-quality claim.

The dwell timing also needs follow-up. The nominal `1 ms` cases reported about `1.70-1.82 s` total for `12000` transitions, or only `142-151 us/transition`. The nominal `10 ms` cases reported about `15.25 s` total for `1200` transitions, or about `12.7 ms/transition`. This means the current dynamic test is useful as a retune stress test, but it is not yet a calibrated dwell-time benchmark.

## Dynamic Host Metric Update

The host dynamic path now records:

- intended carrier 1 exact-marker power/frequency
- intended carrier 2 exact-marker power/frequency
- strongest out-of-band spur after excluding guarded intended-carrier windows
- dynamic spur margin
- firmware-reported elapsed burst time
- measured microseconds per transition

This is the right output format for the next dynamic debugging iteration. The remaining work is now bench interpretation and dwell calibration, not host/firmware marker synchronization.

## Recommended Presentation to Professor

Suggested framing:

1. Lead with the system capability, not the raw RF numbers.

   "We now have automated firmware/host control for dense static DDS characterization and parameterized dynamic retune bursts across the ion-trap operating band."

2. Show the dense DDS-band map as the most mature result.

   Use `dds_band_plot.svg` as the main figure. Present it as a relative amplitude map over `200-300 MHz` at `10 kHz` spacing. Emphasize the number of points and automated capture flow.

3. Treat SFDR and phase-noise as method-validation results.

   The data is useful, but the current sparse SFDR and some phase-noise rows show analyzer carrier-selection problems. Present these as "measurement pipeline established; carrier-locking refinement in progress" rather than as final spectral purity claims.

4. Present dynamic retune as an integration milestone with two explicit caveats.

   Show the UART-confirmed four dynamic cases and the new exact-marker CSV columns. State clearly that parameterized retune execution works, but the current analog margins are negative and the `1 ms` dwell timing is not calibrated yet.

5. Separate publication claims from bench bring-up.

   Publication-ready claim path:
   - dense amplitude map
   - corrected sparse SFDR at locked carriers
   - corrected phase-noise at locked carriers
   - corrected dynamic run after dwell calibration and RF-path isolation
   - long-run CORDIC/scheduler pulse-count demonstration

6. Be explicit about the next actions.

   Recommended immediate next runs:
   - calibrate the dynamic dwell timing and repeat the dynamic-only run
   - rerun sparse SFDR with carrier-locked or narrowed carrier acquisition
   - rerun phase-noise at `200, 250, 300 MHz` after confirming carrier lock
   - run the long-duration pulse-count/scalability benchmark for the CORDIC "unlimited pulses" claim

## Bottom Line

The automation is now strong enough to support the planned evaluation matrix. The dense DDS-band map is the strongest presentable result today. The dynamic run proves the parameterized retune machinery works and now reports exact intended-carrier markers plus measured timing, but the analog margins and dwell calibration are not ready for publication claims. The SFDR and phase-noise numbers should be treated as diagnostic until the carrier-selection path is tightened.
