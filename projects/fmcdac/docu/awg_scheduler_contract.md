# AWG Scheduler Contract (v2)

This document is the software<->HDL contract for the AWG timed-control
peripheral used by `projects/fmcdac/src/app/awg_sched.{h,c}`.

All registers are 32-bit, little-endian at the CPU AXI4-Lite interface. All
offsets are byte addresses relative to the peripheral base address.

The register constants live in
`projects/fmcdac/src/app/awg_sched_regs.h`. When a register-generation flow is
adopted, that header must be regenerated from the same source as the HDL
localparams.

## Register Map

| Offset | Name             | Access | Description |
|--------|------------------|--------|-------------|
| 0x00   | CTRL             | W      | Control strobe register |
| 0x04   | STATUS           | R      | Status and error-code register |
| 0x08   | EVT_COUNT        | RW     | Number of loaded events |
| 0x0C   | CUR_EVT          | R      | Index of currently executing event |
| 0x10   | ERR_REG          | R      | Latched hardware error code |
| 0x14   | IP_ID            | R      | IP magic word (`0x41574753`, `'AWGS'`) |
| 0x18   | IP_VERSION       | R      | Major[31:16] / Minor[15:0] |
| 0x1C   | IP_CAPS          | R      | Capability word |
| 0x20   | TIME_LO          | R      | Tick counter [31:0] |
| 0x24   | TIME_HI          | R      | Tick counter [63:32] |
| 0x28   | LAST_EXEC_LO     | R      | Last-dispatched event tick [31:0] |
| 0x2C   | LAST_EXEC_HI     | R      | Last-dispatched event tick [63:32] |
| 0x30   | COMMIT_COUNT     | R      | Total events dispatched since reset |
| 0x34   | REINIT_COUNT     | R      | Sequence re-start count since reset |
| 0x38   | REINIT_REJECT    | R      | Re-init requests rejected for timing reasons |
| 0x3C   | IRQ_STATUS       | R/W1C  | Latched interrupt flags |
| 0x40   | EVT_WADDR        | W      | Event BRAM write address |
| 0x44   | EVT_WDATA0       | W      | Timestamp [31:0] |
| 0x48   | EVT_WDATA1       | W      | Timestamp [63:32] |
| 0x4C   | EVT_WDATA2       | W      | Channel[31:16] \| Flags[15:0] |
| 0x50   | EVT_WDATA3       | W      | Payload word 0 |
| 0x54   | EVT_WDATA4       | W      | Payload word 1 |
| 0x58   | EVT_WDATA5       | W      | Payload word 2 |
| 0x5C   | EVT_WDATA6       | W      | Payload word 3 |
| 0x60   | EVT_WCTRL        | W      | Write 1 to commit staged event data |
| 0x64   | IRQ_ENABLE       | RW     | Interrupt enable mask |
| 0x68   | IP_SCRATCH       | RW     | Scratch register for firmware debug |
| 0x6C   | TIME_RELOAD_LO   | W      | Epoch reload value low word |
| 0x70   | TIME_RELOAD_HI   | W      | Epoch reload value high word |
| 0x74   | TIME_RELOAD_CTRL | W      | Epoch reload control strobe |

## CTRL Register Bits

| Bit | Name       | Description |
|-----|------------|-------------|
| 0   | RUN        | Start sequence execution |
| 1   | ARM        | Arm hardware trigger gate |
| 2   | STOP_REQ   | Request graceful stop after current event |
| 3   | RESET_SOFT | Pulse to clear state |

Firmware emits ARM and RUN as separate AXI writes with a status round-trip in
between. That avoids simultaneous-edge races in implementations that sample the
request toggles in the same scheduler clock cycle.

## TIME_RELOAD_CTRL Bits

| Bit | Name          | Description |
|-----|---------------|-------------|
| 0   | ARM_ON_SYSREF | Reload epoch on the next SYSREF edge |
| 1   | LOAD_NOW      | Reload epoch immediately from `TIME_RELOAD_LO/HI` |

Current FMCDAC firmware uses `LOAD_NOW` for deterministic host-driven epoch
reload before `RUN`.

## STATUS Register Bits

| Bits | Name    | Description |
|------|---------|-------------|
| 0    | armed   | Hardware trigger gate is armed |
| 1    | running | Sequence is currently executing |
| 2    | done    | Sequence has completed |
| 3    | error   | Hardware error latched |
| 15:8 | err_code| Latched error code |

## IP_CAPS Register

| Bits  | Field          | Description |
|-------|----------------|-------------|
| 31:24 | evt_depth_log2 | `1 << field` gives maximum event depth |
| 23:16 | payload_bits   | Payload width in bits |
| 15:8  | ts_bits        | Timestamp width in bits |
| 7:0   | reserved       | Must be zero |

## IRQ_STATUS Bits

| Bit | Name             | Description |
|-----|------------------|-------------|
| 0   | done_irq         | Sequence-done interrupt |
| 1   | error_irq        | Error interrupt |
| 2   | spacing_viol_irq | Timing-violation interrupt |

`awg_sched_wait_done()` supports an IRQ-driven wait path when
`FMCDAC_AWG_SCHED_USE_IRQ` is enabled. Otherwise firmware uses polling.

## Event Format (v1, 256 bits / 32 bytes)

`awg_event_v1_t`:

| Byte offset | Field           | Type     | Description |
|-------------|-----------------|----------|-------------|
| 0           | timestamp_ticks | uint64_t | 64-bit tick timestamp |
| 8           | channel         | uint16_t | Output channel selector |
| 10          | flags           | uint16_t | Event-type flags |
| 12          | payload         | 4xuint32 | DDS control words |
| 28          | reserved        | uint32_t | Must be zero |

Compile-time ABI guards enforce the offsets and `sizeof(awg_event_v1_t)==32`.

## Event Write Sequence

1. Write `EVT_WADDR = <index>`.
2. Write `EVT_WDATA0..6`.
3. Write `EVT_WCTRL = 1`.

The current HDL does not expose an AXI-visible event-write acknowledge. Host
and firmware therefore pace back-to-back commits conservatively during bring-up
and validation.

## Payload v1 Bit Contract

The active HDL contract is bit-packed DDS control, not the older
`tone/freq16/phase16` format.

- `payload[15:0]` = `scale`
- `payload[16 +: DDS_PHASE_DW]` = `init`
- `payload[16 + DDS_PHASE_DW +: DDS_PHASE_DW]` = `incr`
- upper remaining bits = reserved and must be zero

For the current KCU116 AWG image, `DDS_PHASE_DW=32`, so the 128-bit payload
maps as:

- `word0[15:0]` = `scale[15:0]`
- `word0[31:16]` = `init[15:0]`
- `word1[15:0]` = `init[31:16]`
- `word1[31:16]` = `incr[15:0]`
- `word2[15:0]` = `incr[31:16]`
- `word2[31:16]` and `word3[31:0]` = reserved

## IP Identity Handshake

`awg_sched_config()` performs an identity check before issuing soft reset:

1. read `IP_ID`; fail with `-ENODEV` if not `'AWGS'`
2. read `IP_VERSION`; fail with `-ENOTSUP` on major mismatch
3. read `IP_CAPS`; store event depth / payload bits / timestamp bits

That lets firmware detect a mismatched or absent bitstream before touching the
rest of the block.

## tick_hz

`awg_sched_cfg_t.tick_hz` is informational only. The current contract does not
expose tick rate through `IP_CAPS`, so firmware and host must agree on it
explicitly.

For the current KCU116 image the working value is `100000000`.

## Optional Load Readback Verification

Controlled by `AWG_SCHED_ENABLE_LOAD_READBACK_VERIFY` (default: 0).

This must stay disabled for the current HDL. Reads from `EVT_WDATA0..6` return
the last staged write registers, not indexed event-memory contents, so naive
post-load compare logic produces false mismatches after the first event.

## Host Upload Transport

The current UART console transport accepts `LOADBIN <count>` followed by the
packed event blob encoded as ASCII hex, not raw binary.

Expected console sequence:

1. `[AWG-UART] LOADBIN READY ...`
2. `[AWG-UART] LOADBIN RX BEGIN bytes=<n> hex_chars=<2n>`
3. `[AWG-UART] LOADBIN RX OK ...`
4. `[AWG-UART] LOADBIN COMMIT BEGIN ...`
5. `[AWG-UART] LOADBIN OK ...`

## Host Measured-AWG Policy

The uploaded AWG path supports two host-side validation modes:

1. control-plane validation only
2. measured validation with an analyzer

For measured validation the host:

1. waits for the UART `set_epoch` artifact line
2. anchors measurement timing to that epoch, not to the host `RUN` write
3. uses a narrow expected-tone-centered window for dense adjacent sweeps

This is required because anchoring to the host `RUN` write races firmware-side
pre-run work such as link checks, DAC/GPIO setup, `set_nco`, and epoch reload.

## Event Depth Limitation

The current KCU116 image reports `max_events=64`.

That is enough for short deterministic sequences and coarse stepped sweeps, but
not for dense one-shot benches such as `200-300 MHz` in `10 kHz` steps
(`~10001` events). Those workflows require:

1. host-side batching across multiple scheduler runs
2. a larger HDL event RAM
3. or a future streamed / DMA-backed scheduler architecture

## Artifact Dump

`awg_sched_dump_artifacts(events, count, status)` emits machine-parsable lines
prefixed `[SCHED-ARTIFACT]`:

```text
[SCHED-ARTIFACT] config base=0x44AA0000 max_events=64 tick_hz=100000000 timeout_ms=2000
[SCHED-ARTIFACT] event idx=0 ts=0x00000000_000003E8 ch=0 fl=0x0001 p0=0x00010000 p1=... p2=... p3=...
[SCHED-ARTIFACT] status armed=1 running=0 done=1 error=0 err_code=0x00 current=4 loaded=4 commit=4 reinit=0 reinit_reject=0 irq=0x00000001
[SCHED-ARTIFACT] time_now=0x00000000_00001388 last_exec=0x00000000_00000FA0
```

Current host policy treats `error=0` plus `commit_count >= loaded_events` as a
successful terminal condition even if a stale snapshot still shows
`running=1 done=0`. That is a known status-snapshot limitation in the current
HDL, not an execution failure.

## Host-Side Unit Tests

`projects/fmcdac/tests/` contains a host-buildable test suite covering the
`AWG_EVTVAL_ERR_*` paths. Build and run with:

```sh
make -C projects/fmcdac/tests run
```
