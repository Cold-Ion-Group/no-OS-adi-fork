# M4i66 + FSH/MSO22 Benchmark Script

`run_m4i66_fsh_bench.py` benchmarks a Spectrum `M4i.66xx` AWG with the same
FSH-driven analog checks already used for FMCDAC, then adds M4i-specific
control-path, replay-memory, and MSO22 pulse-switching stress tests.

Sections:

1. `DDS-band` analog sweep on the FSH
2. `SFDR` analog sweep on the FSH
3. host-driven `dynamic_sfdr` retune bursts on the FSH
4. DDS update latency / update-rate benchmarks
5. DDS queue-pressure probe under dense workloads
6. replay-memory upload bandwidth benchmarks
7. pulse-switching limit plus replay-memory limit at that rate

Important scope note:

1. the DDS update-latency numbers are host-to-card commit timings
2. they are not direct analog propagation latencies
3. PulseGen is not used; the pulse-switching bench uses DDS command mode when
   available, otherwise it falls back to AWG replay-memory pulse trains
4. the MSO22 pulse-switching path does not require the FSH; skip the FSH
   sections and omit `--visa-resource`

The pulse-switch artifact reports:

1. live DDS edge update rate when DDS command mode is available, otherwise the
   replay-generated edge and pulse-cycle rate
2. optional MSO22 envelope-derived pulse frequency and duty cycle
3. theoretical replay-memory pulse-cycle rate from sample rate and minimum
   high/low sample counts
4. DDS queue depth converted to queued pulse cycles when DDS mode is available
5. whether the onboard replay memory can hold the requested finite duration at
   the modeled switching frequency

Example:

```powershell
python .\run_m4i66_fsh_bench.py `
  --card-identifier "/dev/spcm0" `
  --visa-resource "TCPIP::192.168.100.142::INSTR" `
  --visa-backend "@py" `
  --sample-rate-hz 625000000 `
  --rbw-hz 100000 `
  --vbw-hz 100000 `
  --dynamic-trace-mode maxhold
```

Useful options:

```powershell
--skip-dds-band-test
--skip-sfdr-test
--skip-dynamic-sfdr-test
--skip-update-benchmarks
--skip-queue-probe
--skip-memory-benchmarks
--skip-pulse-switch-benchmark
--pulse-switch-mode auto
--pulse-switch-replay-waveform square
--dense-spacing-hz 500000
--dense-update-count 200
--paced-update-interval-us 1000
--memory-bench-samples 8388608
--dense-memory-bench-repeats 16
--pulse-switch-carrier-hz 100000000
--pulse-switch-toggles 1000
--pulse-switch-target-duration-s 1
--pulse-switch-upload-max-samples 1048576
```

Pulse-switching only with MSO22, no FSH required:

```powershell
python .\run_m4i66_fsh_bench.py `
  --card-identifier "/dev/spcm0" `
  --scope-visa-resource "TCPIP::192.168.100.143::INSTR" `
  --scope-visa-backend "@py" `
  --skip-dds-band-test `
  --skip-sfdr-test `
  --skip-dynamic-sfdr-test `
  --skip-update-benchmarks `
  --skip-queue-probe `
  --skip-memory-benchmarks `
  --pulse-switch-mode auto `
  --pulse-switch-replay-waveform square `
  --pulse-switch-carrier-hz 100000000 `
  --pulse-switch-toggles 2000 `
  --scope-channel CH1 `
  --scope-capture-s 0.2 `
  --scope-vertical-scale-v 0.2
```

For more reliable MSO22 envelope detection, use a carrier comfortably inside
the scope bandwidth or route an envelope/marker signal to the scope. The
automation thresholds the rectified waveform envelope, so direct RF captures
near the scope bandwidth can be ambiguous.

Artifacts:

1. `summary.json`
2. per-step `stepNN_*.json` and `stepNN_*.csv`
3. `sfdr_results.csv`
4. `dynamic_sfdr_results.csv`
5. `update_benchmarks.json`
6. `queue_probe.json`
7. `memory_benchmarks.json`
8. `pulse_switch_benchmark.json`
