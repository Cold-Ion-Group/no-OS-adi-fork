# Build, Program, and Use the Full AWG System

This guide covers the KCU116, AD9144 FMC DAC, scheduler DMA, and 10G Ethernet
firmware. Run all commands on the licensed build or lab machine. Keep the HDL,
XSA, bitstream, firmware ELF, and logs from one source revision together.

## 1. Repository Map

Keep both repositories side by side in a path without spaces:

```text
<workspace>/
├─ hdl-adi-fork/
│  └─ projects/awg/
│     ├─ BUILD_AND_USE.md             HDL build guide
│     ├─ build_awg_kcu116.ps1         complete HDL wrapper
│     ├─ common/                       scheduler and extension RTL
│     └─ kcu116/                       board design and Vivado scripts
└─ no-OS-adi-fork/
   └─ projects/fmcdac/
      ├─ BUILD_AND_USE.md              this guide
      ├─ Makefile                      firmware profiles
      ├─ src/app/                      target firmware
      ├─ tests/                        host-only tests
      ├─ build_phase_f_firmware.ps1    clean firmware build wrapper
      ├─ build_phase_f_profiles.ps1    build all five release profiles
      ├─ program_phase_f_firmware.ps1  program an existing XSA and ELF
      ├─ collect_phase_f_artifacts.ps1 collect a closure bundle
      ├─ gen_manifest.ps1              hash build inputs and outputs
      ├─ configure_awg_nic.ps1         Windows 10G NIC setup
      ├─ capture_phase_f_uart.py       raw and timestamped UART capture
      ├─ awg_stream_sender_v2.py       production GWAS/2 sender
      ├─ awg_c1_program.py             finite C1 program generator
      ├─ requirements-host.txt         host Python packages
      └─ awg_stream_sender.py          legacy GWAS/1 diagnostic sender
```

Useful source files:

| Path | Purpose |
| --- | --- |
| `src.mk` | Firmware source list |
| `src/app/app_config.h` | Feature and network defaults |
| `src/app/parameters.h` | XSA/XPAR, IRQ, and DDR contract |
| `src/app/awg_sched_regs.h` | Firmware copy of the scheduler ABI |
| `src/app/awg_stream_proto.h` | GWAS/1 and GWAS/2 wire definitions |
| `docu/PHASE_F_FIRMWARE_CLOSURE_REPORT.md` | Current source and hardware status |

## 2. Build-Machine Requirements

Install:

- Git and access to the private fork submodules.
- Windows PowerShell and GNU Make.
- An MSYS2, MinGW, or WSL shell with host GCC and core utilities for host-only
  C tests.
- Vivado and Vitis 2021.2, including XSCT and MicroBlaze GCC.
- KCU116 board files and cable drivers.
- A bitstream-generation license for XXV Ethernet v4.0.
- Python 3.10 or newer.

Install host Python packages:

```powershell
python -m pip install -r .\projects\fmcdac\requirements-host.txt
```

`pyserial` is required. The analyzer packages in that file are optional.

Hardware needed for the full Ethernet path:

- KCU116 and AD9144 FMC DAC board.
- JTAG and USB UART connections.
- SFP0, compatible SFP+ modules or cable, and a 10G host NIC.

Do not put either checkout or the XSA in a path containing spaces. Keep build
artifacts outside both Git worktrees, for example `D:\awg-artifacts`.

## 3. Fresh Checkout

Clone the approved forks with submodules:

```powershell
git clone --recurse-submodules <hdl-fork-url> hdl-adi-fork
git clone --recurse-submodules <no-os-fork-url> no-OS-adi-fork
git -C .\hdl-adi-fork submodule update --init --recursive
git -C .\no-OS-adi-fork submodule update --init --recursive
```

Record both repository commits before a release build:

```powershell
git -C .\hdl-adi-fork rev-parse HEAD
git -C .\no-OS-adi-fork rev-parse HEAD
git -C .\hdl-adi-fork submodule status --recursive
git -C .\no-OS-adi-fork submodule status --recursive
```

Both release worktrees must be clean. `git status --short` must print nothing.
Commit or stash local changes before building. The wrappers reject dirty source
unless `-AllowDirtySource` is passed for a development-only build.

## 4. Build the HDL

Read `hdl-adi-fork/projects/awg/BUILD_AND_USE.md` first. From the HDL repo:

```powershell
Set-Location <workspace>\hdl-adi-fork

powershell -ExecutionPolicy Bypass `
  -File .\projects\awg\build_awg_kcu116.ps1 `
  -Variant C1 `
  -Jobs 4 `
  -ArtifactRoot D:\awg-artifacts
```

Use `-Variant Direct` when the C1 decoder is not required. The C1 and Direct
builds keep the same register addresses. The wrapper packages the ADI IP,
runs the Phase E gates, and fails unless both files exist:

```text
system_top.bit
system_top.xsa
```

The XSA must include the bitstream. An unlicensed routed design is not a usable
firmware handoff.

## 5. Confirm the XSA Contract

The generated `xparameters.h` must describe this hardware:

| Core | Address | MicroBlaze IRQ |
| --- | ---: | ---: |
| AWG scheduler | `0x44AA0000` | 14 |
| Scheduler DMAC | `0x44AB0000` | 12 |
| Ethernet MAC | `0x44C00000` | polled |
| Ethernet RX DMAC | `0x44AC0000` | 10 |
| Ethernet TX DMAC | `0x44AD0000` | 9 |
| AWGX extension | `0x44AE0000` | polled |

Enabled firmware profiles fail to compile when generated addresses or IRQs do
not match. Do not replace a mismatch with an unchecked constant. Use the XSA
from the matching HDL build.

The firmware reserves DDR for:

```text
DDR + 0x00800000  existing DAC waveform data
DDR + 0x01000000  1 MiB scheduler event ring
after the ring    two 9216-byte RX buffers, then two 9216-byte TX buffers
```

Review the generated linker script and DDR high address before board use.

## 6. Choose a Firmware Profile

| Profile | Use |
| --- | --- |
| `default` | Existing DDS/JESD bring-up |
| `scheduler-preload` | Scheduler preload and UART console |
| `scheduler-stream` | Adds UART `STREAMHEX` fallback |
| `scheduler-dma` | Adds scheduler DMAC and IRQ refill |
| `scheduler-eth` | Full scheduler DMA and 10G UDP path |

Bring up a new XSA in that order. Do not start with Ethernet when the DDS/JESD
or scheduler probes are failing.

## 7. Build the Firmware

From the no-OS repository, use the wrapper with the XSA copied by the HDL step:

```powershell
Set-Location <workspace>\no-OS-adi-fork

powershell -ExecutionPolicy Bypass `
  -File .\projects\fmcdac\build_phase_f_firmware.ps1 `
  -XsaPath D:\awg-artifacts\20260721_120000_123_awg_kcu116-c1\system_top.xsa `
  -Profile scheduler-eth `
  -XilinxSettings C:\Xilinx\Vitis\2021.2\settings64.bat `
  -ArtifactRoot D:\awg-artifacts `
  -Jobs 4
```

The wrapper always removes the old generated BSP and application before it
builds. This prevents an old XSA or profile from leaving stale flags behind.
It creates:

```text
projects/fmcdac/build/fmcdac.elf
projects/fmcdac/build/tmp/system_top.xsa
projects/fmcdac/build/bsp/.../include/xparameters.h
projects/fmcdac/build/app/src/lscript.ld
```

It also copies the ELF, XSA, generated XPAR header, linker script, build log,
and manifest into the external artifact directory.

The clean wrapper refuses a local `fmcdac_build.env`. Use the manual procedure
for custom constants and archive that file in the manifest.

Build all five profiles from the same XSA for a release handoff:

```powershell
powershell -ExecutionPolicy Bypass `
  -File .\projects\fmcdac\build_phase_f_profiles.ps1 `
  -XsaPath D:\awg-artifacts\20260721_120000_123_awg_kcu116-c1\system_top.xsa `
  -XilinxSettings C:\Xilinx\Vitis\2021.2\settings64.bat `
  -ArtifactRoot D:\awg-artifacts `
  -Jobs 4
```

### Manual build

Start a Vitis 2021.2 Command Prompt, enter `powershell`, then copy and edit the
local profile file:

```powershell
Set-Location <workspace>\no-OS-adi-fork\projects\fmcdac
Copy-Item .\fmcdac_build.env.example .\fmcdac_build.env
```

Set `FMCDAC_XSA` and `FMCDAC_AWG_PROFILE`, then run:

```powershell
$ManualArtifactDir = 'D:\awg-artifacts\manual-scheduler-eth'
$BuildLog = Join-Path $ManualArtifactDir 'build.log'
New-Item -ItemType Directory -Force -Path $ManualArtifactDir | Out-Null

Copy-Item `
  D:\awg-artifacts\20260721_120000_123_awg_kcu116-c1\system_top.xsa `
  .\system_top.xsa

make SKIP_MANIFEST=1 fmcdac-build-config 2>&1 |
  Tee-Object -FilePath $BuildLog
if ($LASTEXITCODE -ne 0) { throw 'Build configuration failed.' }

make SKIP_MANIFEST=1 reset 2>&1 |
  Tee-Object -FilePath $BuildLog -Append
if ($LASTEXITCODE -ne 0) { throw 'Build reset failed.' }

make SKIP_MANIFEST=1 FMCDAC_AWG_PROFILE=scheduler-eth 2>&1 |
  Tee-Object -FilePath $BuildLog -Append
if ($LASTEXITCODE -ne 0) { throw 'Firmware build failed.' }

powershell -ExecutionPolicy Bypass -File .\gen_manifest.ps1 `
  -XsaPath .\system_top.xsa `
  -ElfPath .\build\fmcdac.elf `
  -XparametersPath .\build\bsp\sys_mb\include\xparameters.h `
  -LinkerScriptPath .\build\app\src\lscript.ld `
  -BuildLogPath $BuildLog `
  -BuildConfigPath .\fmcdac_build.env `
  -Profile scheduler-eth `
  -OutputPath (Join-Path $ManualArtifactDir 'manifest.json')
```

Use `make reset` after every XSA or profile change. Use the same profile for
build and programming. Pass `-BuildConfigPath .\fmcdac_build.env` to the
artifact collector for a manual build.

## 8. Program the Board

Build and program in one step by adding `-Program` to the firmware wrapper.
When more than one JTAG cable is connected, also pass `-JtagCableId`.

Run the commands below from the no-OS repository root:

```powershell
Set-Location <workspace>\no-OS-adi-fork
```

To program an existing matching XSA and ELF without rebuilding:

```powershell
powershell -ExecutionPolicy Bypass `
  -File .\projects\fmcdac\program_phase_f_firmware.ps1 `
  -XsaPath D:\awg-artifacts\20260721_120000_123_awg_kcu116-c1\system_top.xsa `
  -ElfPath D:\awg-artifacts\20260721_130000_456_scheduler-eth\firmware\fmcdac.elf `
  -ManifestPath D:\awg-artifacts\20260721_130000_456_scheduler-eth\manifest.json `
  -XilinxSettings C:\Xilinx\Vitis\2021.2\settings64.bat `
  -LogPath D:\awg-artifacts\program.log
```

This programs the bitstream contained in the XSA, downloads the ELF to the
MicroBlaze, and starts it. Never combine an ELF with a different XSA or
bitstream.

The programming wrapper is preferred because it verifies the XSA and ELF
against the selected manifest before it opens JTAG.

## 9. Capture UART and Bring Up Each Layer

UART settings are 115200 baud, 8 data bits, no parity, one stop bit.

Start a raw and timestamped capture:

```powershell
python .\projects\fmcdac\capture_phase_f_uart.py `
  --serial-port COM4 `
  --output D:\awg-artifacts\runtime\boot `
  --interactive
```

The script creates `boot.raw` and `boot.log`. Stop it with Ctrl+C, or add
`--duration 120`. With `--interactive`, type commands in the same window. Type
`/quit` to stop without sending that text to the firmware.

Scheduler console commands:

```text
INFO
STATUS
LOADBIN <count>
RUN
ABORT
DUMP
STREAMINFO
STREAMSTATUS
STREAMRESET
STREAMHEX <bytes>
EXIT
```

For a preload smoke run, stop any other program using the COM port, then run:

```powershell
python .\projects\fmcdac\awg_sweep_test.py `
  --serial-port COM4 `
  --skip-make-run `
  --awg-sweep-start-hz 10000000 `
  --awg-sweep-stop-hz 12000000 `
  --awg-sweep-step-hz 1000000 `
  --awg-sweep-dwell-us 100000 `
  --output-dir D:\awg-artifacts\runtime\preload-smoke
```

For `scheduler-stream`, run the existing UART stream smoke:

```powershell
python .\projects\fmcdac\run_nco_scope_test.py `
  --serial-port COM4 `
  --skip-make-run `
  --run-scheduler-benchmark-suite `
  --scheduler-suite-profile stream-bringup `
  --scheduler-transport stream `
  --output-dir D:\awg-artifacts\runtime\stream-smoke
```

Run the same stream command with the `scheduler-dma` firmware profile to test
the DMA refill path. Keep its scheduler-DMAC EOT/error output.

Bring-up order:

1. Confirm AD9516 lock, AD9144 setup, JESD DATA state, and DDS output.
2. Confirm scheduler `IP_ID`, `IP_VERSION`, and `STREAM_DEPTH`.
3. Run a finite preload sequence.
4. Run a finite software-stream sequence ending in EOF.
5. Run scheduler-DMA refill and confirm EOT with no scheduler error.
6. Confirm the SFP0 MAC reports link and block lock without local or remote
   fault.
7. Start UDP traffic.

## 10. Configure the Host 10G NIC

Open an elevated PowerShell window. Preview the change first:

```powershell
.\projects\fmcdac\configure_awg_nic.ps1 `
  -InterfaceAlias 'Ethernet 2' `
  -StateFile D:\awg-artifacts\nic-state.json `
  -WhatIf
```

Apply it by removing `-WhatIf`. Defaults are:

```text
host IPv4  192.0.2.1/24
FPGA IPv4  192.0.2.2
FPGA MAC   02:00:00:00:00:02
UDP port   5000
```

Remove only the address and route added by the script:

```powershell
.\projects\fmcdac\configure_awg_nic.ps1 `
  -InterfaceAlias 'Ethernet 2' `
  -StateFile D:\awg-artifacts\nic-state.json `
  -Remove
```

Make sure no other interface owns `192.0.2.0/24`.

The NIC helper does not change MTU. The sender defaults to MTU 1500 and keeps
each UDP datagram unfragmented.

## 11. Send a Production GWAS/2 Program

Send deterministic direct events and wait for every ACK:

```powershell
python .\projects\fmcdac\awg_stream_sender_v2.py `
  --kind direct `
  --source-ip 192.0.2.1 `
  --dest-ip 192.0.2.2 `
  --port 5000 `
  --count 1024 `
  --tick-step 24576 `
  --scale 0x4000 `
  --phase-increment 0x01000000 `
  --batch 45 `
  --mtu 1500 `
  --rate 0 `
  --ack-timeout 0.25 `
  --retries 3 `
  --telemetry D:\awg-artifacts\host\gwas2-direct.json
```

For an existing direct-event file, pass `--records-file`. The file must contain
only concatenated 32-byte `awg_event_v1_t` records.

For C1, build the C1 HDL variant. Generate a finite LINEAR program whose last
decoded event carries EOF:

```powershell
python .\projects\fmcdac\awg_c1_program.py `
  --output D:\awg-programs\program.c1.bin `
  --metadata D:\awg-programs\program.c1.json `
  --count 1024 `
  --start-ticks 61440000 `
  --dwell-ticks 24576 `
  --scale 0x4000 `
  --phase-increment 0x01000000
```

Then send its 32-byte C1 records:

```powershell
python .\projects\fmcdac\awg_stream_sender_v2.py `
  --kind c1 `
  --records-file D:\awg-programs\program.c1.bin `
  --source-ip 192.0.2.1 `
  --dest-ip 192.0.2.2 `
  --batch 45 `
  --mtu 1500 `
  --telemetry D:\awg-artifacts\host\gwas2-c1.json
```

The sender opens one session with a SHA-256 program identity, uses strict
sequence numbers, retries only the identical datagram, and closes the stream.
Keep its telemetry with the UART log.

At MTU 1500, do not use a batch larger than 45 records. To use a larger batch,
configure the same jumbo MTU on the host link and pass that value with `--mtu`.
The firmware rejects fragmented IPv4 packets.

Generated direct events get an automatic upload margin before the first event.
The raw DDS defaults are zero, so pass nonzero `--scale` and
`--phase-increment` for an analog-output run.

A successful CLOSE ACK means the program was accepted. It does not prove that
the last event fired. Confirm EOF, DONE, and error counters in the UART log.

`awg_stream_sender.py` uses GWAS/1. Use it only for compatibility diagnostics.

## 12. Recovery

| Problem | Action |
| --- | --- |
| XPAR address or IRQ mismatch | Stop and rebuild with the correct XSA. |
| Scheduler or extension ID mismatch | Reprogram the matching XSA and ELF. |
| No Ethernet link | Check SFP0, NIC mode, cable, block lock, and MAC faults. |
| Lost UDP ACK | Retry the exact same frame. Do not change its payload. |
| Ring full | Wait for space and retry the exact same frame. |
| Scheduler or DMA error | Save status, use `STREAMRESET`, then open a new session at sequence 0. |
| Reset does not recover | Reprogram the matching bundle or power-cycle the board. |

Do not use `CTRL.STOP` as a FIFO flush. `STREAMRESET` uses the scheduler soft
reset and clears the FIFO.

## 13. Host-Only Tests on the Build Machine

Run these commands in an MSYS2, MinGW, or WSL shell. They do not replace board
validation:

```powershell
make -C projects/fmcdac/tests run
make -C projects/fmcdac/tests run-host
make -C projects/fmcdac/tests clean
```

They cover the scheduler model, GWAS/1, GWAS/2, ring/DMA policy, XPAR fixture,
network parsing, and Ethernet I/O state machines.

## 14. Collect a Full Artifact Bundle

After a board run:

```powershell
.\projects\fmcdac\collect_phase_f_artifacts.ps1 `
  -ArtifactRoot D:\awg-artifacts `
  -HdlRepo <workspace>\hdl-adi-fork `
  -FirmwareRepo <workspace>\no-OS-adi-fork `
  -XsaPath D:\awg-artifacts\20260721_120000_123_awg_kcu116-c1\system_top.xsa `
  -BitPath D:\awg-artifacts\20260721_120000_123_awg_kcu116-c1\system_top.bit `
  -HdlManifestPath D:\awg-artifacts\20260721_120000_123_awg_kcu116-c1\awg_kcu116_c1_manifest.json `
  -ElfPath D:\awg-artifacts\20260721_130000_456_scheduler-eth\firmware\fmcdac.elf `
  -XparametersPath D:\awg-artifacts\20260721_130000_456_scheduler-eth\firmware\xparameters.h `
  -LinkerScriptPath D:\awg-artifacts\20260721_130000_456_scheduler-eth\firmware\lscript.ld `
  -BuildLogPath D:\awg-artifacts\20260721_130000_456_scheduler-eth\firmware\build.log `
  -FirmwareManifestPath D:\awg-artifacts\20260721_130000_456_scheduler-eth\manifest.json `
  -Profile scheduler-eth `
  -UartLog D:\awg-artifacts\runtime\boot.log `
  -UartRaw D:\awg-artifacts\runtime\boot.raw `
  -ProgramLog D:\awg-artifacts\program.log `
  -HostTelemetry @(
    'D:\awg-artifacts\host\gwas2-direct.json',
    'D:\awg-artifacts\host\gwas2-c1.json'
  )
```

The bundle contains source revisions, submodule revisions, tool versions,
hardware files, ABI headers, generated firmware files, runtime logs, telemetry,
and `SHA256SUMS`. Omit optional UART, programming, or telemetry paths that were
not created. A manual build must also pass
`-BuildConfigPath <workspace>\no-OS-adi-fork\projects\fmcdac\fmcdac_build.env`.

## 15. Hardware Closure Order

Record pass or fail for each step:

1. Default DDS/JESD boot.
2. Scheduler preload finite run.
3. Software stream finite EOF run.
4. Scheduler-DMA finite EOF and ring-wrap run.
5. Ethernet link, ARP, RX DMA, and TX DMA.
6. GWAS/2 direct end to end.
7. GWAS/2 C1 end to end when using the C1 build.
8. Increasing-rate run with queue and DMA counters.
9. Long soak with zero scheduler errors and bounded backlog.

Do not claim the 130 ns event-spacing target until transport, queue, timing,
and analog evidence are all captured.
