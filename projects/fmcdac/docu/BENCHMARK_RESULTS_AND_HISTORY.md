# Benchmark Results And History

Date: 2026-04-02

This note collects the measurement history that led to the current working
conclusion.

## Measurement Story

The evaluation has had two distinct phases:

1. Scope-based investigation on the Tek MSO22
2. Spectrum-analyzer-based investigation on the R&S FSH8

The biggest conclusion change happened between those two phases.

## Phase 1: MSO22 Scope Result

Original observation on the MSO22:

1. waveform amplitude started to look weak around `250-270 MHz`
2. waveform was reported as effectively gone above `~290 MHz`

Important later conclusion:

1. the MSO22 bench unit had only `200 MHz` bandwidth
2. later analyzer measurements did not reproduce the severe collapse
3. that earlier result is now treated as a measurement artifact, not a current
   blocker on the DDS path

## Phase 2: FSH8 Analyzer Runs

### Run `20260402T011521Z`

Artifacts: not retained (superseded by later runs).

What it showed:

1. DDS-band droop was mild, not catastrophic
2. NCO translated tone still looked weak
3. SFDR, throughput, and UART RTT were not yet captured in that run

Interpretation:

1. this was the first strong evidence that the MSO22 result was misleading

### Run `20260402T012530Z`

Artifacts: not retained (superseded by later runs).

What it showed:

1. essentially the same DDS-band result as `20260402T011521Z`
2. still no full SFDR/throughput/UART coverage

Interpretation:

1. strengthened the conclusion that DDS-band amplitude collapse was not real in
   the way the scope suggested

### Run `20260402T022105Z`

Artifacts: not retained (superseded by `20260402T032626Z`).

What it showed:

1. first complete run covering:
   - DDS-band
   - SFDR
   - throughput
   - UART RTT
2. DDS-band still looked healthy
3. throughput and UART RTT were usable
4. SFDR was invalid because every result came out as `0 dBc`

Why SFDR was invalid:

1. the host logic reacquired the carrier as the spur
2. then a follow-on attempt to force wideband trace capture hit
   `TRAC:DATA? TRACE1` timeouts on the FSH8

Interpretation:

1. DDS-band, throughput, and UART RTT were already useful
2. SFDR automation still needed work

### Run `20260402T032626Z`

Artifacts:

1. [summary.json](../capture_runs/20260402T032626Z/summary.json)
2. [sfdr_results.csv](../capture_runs/20260402T032626Z/sfdr_results.csv)
3. [throughput.json](../capture_runs/20260402T032626Z/throughput.json)
4. [uart_rtt.json](../capture_runs/20260402T032626Z/uart_rtt.json)
5. [uart.log](../capture_runs/20260402T032626Z/uart.log)

This is the current primary baseline.

## DDS-Band Result Summary

### Latest baseline: `20260402T032626Z`

Relative to the `10 MHz` reference:

| Frequency | Delta |
|-----------|-------|
| 100 MHz | -0.166 dB |
| 200 MHz | -0.587 dB |
| 230 MHz | -0.818 dB |
| 240 MHz | -1.185 dB |
| 250 MHz | -1.510 dB |
| 260 MHz | -1.621 dB |
| 270 MHz | -1.552 dB |
| 280 MHz | -1.360 dB |
| 290 MHz | -1.244 dB |
| 300 MHz | -1.422 dB |
| 310 MHz | -1.889 dB |
| 320 MHz | -2.423 dB |
| 330 MHz | -2.768 dB |

Interpretation:

1. DDS-band amplitude is not flat, but it is far from "collapsed"
2. carrier frequency remains correct through the whole test region
3. this is consistent with a functioning DDS path and inconsistent with the
   original scope-based disappearance claim

## SFDR Result Summary

### Latest baseline: `20260402T032626Z`

From [sfdr_results.csv](../capture_runs/20260402T032626Z/sfdr_results.csv):

| Carrier | Carrier Power | Worst Spur | Spur Frequency | SFDR |
|---------|---------------|------------|----------------|------|
| 50 MHz | -1.787 dBm | -61.743 dBm | 729.143 MHz | 59.956 dBc |
| 100 MHz | -2.166 dBm | -60.212 dBm | 300.130 MHz | 58.046 dBc |
| 150 MHz | -2.195 dBm | -57.565 dBm | 449.473 MHz | 55.370 dBc |
| 200 MHz | -2.591 dBm | -60.028 dBm | 601.000 MHz | 57.437 dBc |
| 250 MHz | -3.504 dBm | -58.698 dBm | 749.479 MHz | 55.194 dBc |
| 300 MHz | -3.416 dBm | -57.289 dBm | 900.286 MHz | 53.873 dBc |
| 350 MHz | -4.505 dBm | -56.891 dBm | 700.686 MHz | 52.386 dBc |
| 400 MHz | -5.659 dBm | -54.251 dBm | 799.715 MHz | 48.592 dBc |

Interpretation:

1. the SFDR flow is now producing structured, nonzero results
2. many detected worst spurs are harmonic-looking and therefore at least
   physically plausible
3. the measured baseline is still well below the long-term target of
   `>= 85-90 dBc`
4. this is now the main RF-quality issue to refine

Cautions:

1. these values should still be treated as preliminary bench baselines
2. analyzer setup, attenuation, and sweep choices can influence which spur is
   detected as dominant
3. the `50 MHz` case in particular still deserves a sanity check because the
   reported worst spur near `729 MHz` is less obviously aligned with the simple
   harmonic pattern seen in the other rows

## Throughput Baseline Summary

### Run `20260402T022105Z`

1. `axi_mmio_write`: `736,980 ops/s`
2. `ad9144_spi_write`: `5,945 ops/s`
3. `dds_pair_update`: `838 ops/s`

### Run `20260402T032626Z`

1. `axi_mmio_write`: `736,980 ops/s`
2. `ad9144_spi_write`: `5,945 ops/s`
3. `dds_pair_update`: `838 ops/s`

Interpretation:

1. the throughput baseline is stable across the two full runs
2. SPI and DDS retune paths are much slower than raw AXI MMIO

## UART RTT Baseline Summary

### Run `20260402T022105Z`

1. min `3161.9 us`
2. avg `9128.9 us`
3. max `50633.1 us`
4. two large outliers near `50 ms`

### Run `20260402T032626Z`

1. min `3170.6 us`
2. avg `3455.8 us`
3. max `5125.6 us`
4. no giant outliers

Interpretation:

1. typical RTT is roughly low-single-digit milliseconds
2. the later run looks like the cleaner baseline to keep

## Current Historical Conclusion

Across the current measurement history:

1. the DDS-band "collapse" story has been effectively refuted by the FSH8
2. throughput and UART latency baselines are now established
3. SFDR is the active open metric
4. NCO has become secondary to the main DDS evaluation path
