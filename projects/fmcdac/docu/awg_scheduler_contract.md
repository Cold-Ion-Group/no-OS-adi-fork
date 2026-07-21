# AWG Scheduler Contract (v4)

This document is the software<->HDL contract for the AWG timed-control
peripheral used by `projects/fmcdac/src/app/awg_sched.{h,c}`.

All registers are 32-bit, little-endian at the CPU AXI4-Lite interface. All
offsets are byte addresses relative to the peripheral base address.

The register constants live in
`projects/fmcdac/src/app/awg_sched_regs.h`. That header is currently mirrored
byte-for-byte from `hdl-adi-fork/projects/awg/common/awg_sched_regs.h`.

The current KCU116 AWG image exposes the scheduler at `0x44AA0000`, reports
`IP_ID = 0x41574753`, and reports `IP_VERSION = 0x00010000`.

## Register Map

| Offset | Name             | Access | Description |
|--------|------------------|--------|-------------|
| 0x00   | CTRL             | W/R    | Control strobe register plus IRQ gate readback |
| 0x04   | STATUS           | R      | Status and error-code register |
| 0x08   | EVENT_COUNT      | RW     | Legacy fixed-length event count |
| 0x0C   | CUR_EVENT        | R      | Index of currently executing event |
| 0x10   | ERR_REG          | R      | Latched hardware error code |
| 0x14   | IP_ID            | R      | IP magic word (`0x41574753`, `'AWGS'`) |
| 0x18   | IP_VERSION       | R      | ABI version (`0x00010000`) |
| 0x1C   | IP_CAPS          | R      | Capability word |
| 0x20   | TIME_NOW_LO      | R      | Tick counter [31:0] |
| 0x24   | TIME_NOW_HI      | R      | Tick counter [63:32] |
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
| 0x78   | STREAM_CTRL      | RW/W1C | Stream mode control and stream sticky flags |
| 0x7C   | OCCUPANCY        | R      | Stream FIFO occupancy in events |
| 0x80   | FREE_SPACE       | R      | Stream FIFO free space in events |
| 0x84   | LOW_WMARK        | RW     | Low-watermark threshold in events |
| 0x88   | STREAM_DEPTH     | R      | Stream FIFO capacity in events |
| 0x8C   | STREAM_PUSHES    | R      | Accepted stream pushes since reset |
| 0x90   | STREAM_STALLS    | R      | Scheduler cycles spent waiting on an empty stream FIFO |

## CTRL Register Bits

| Bit | Name       | Description |
|-----|------------|-------------|
| 0   | RUN        | Start sequence execution |
| 1   | ARM        | Arm hardware trigger gate |
| 2   | STOP       | Stop or abort execution; does not flush stream FIFO |
| 3   | RESET_SOFT | Pulse to clear state |
| 8   | IRQ_ENABLE | Top-level interrupt gate |

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

The active ABI is:

| Bits | Name    | Description |
|------|---------|-------------|
| 0    | armed   | Hardware trigger gate is armed |
| 1    | running | Sequence is currently executing |
| 2    | done    | Sequence has completed |
| 3    | error   | Hardware error latched |
| 15:8 | err_code| Documented error-code field |

Idle is zero: none of the four state bits is set. Current RTL also exposes a
known status-snapshot error byte in bits `23:16`; firmware falls back to that
byte when the documented field is zero. `ERR_REG` remains the preferred error
source.

Error codes:

| Code | Name |
|------|------|
| 0x00 | NONE |
| 0x01 | MISSED_DEADLINE |
| 0x02 | SPACING_VIOLATION |
| 0x03 | REINIT_SPACING |

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
| 0   | DONE             | Sequence-done interrupt |
| 1   | ERROR            | Hard scheduler error |
| 2   | SPACING_VIOLATION| Timing-violation interrupt |
| 3   | UNDERRUN         | Missed-deadline companion IRQ |
| 4   | LOW_WATERMARK    | Stream FIFO crossed downward to low watermark |
| 5   | EMPTY_STALL      | Stream engine entered empty wait |

`awg_sched_wait_done()` supports an IRQ-driven wait path when
`FMCDAC_AWG_SCHED_USE_IRQ` is enabled. Otherwise firmware uses polling.

`LOW_WATERMARK` is a sticky downward-crossing event, not a level request.
Firmware sets a bounded refill-pending flag in the ISR and also uses periodic
foreground refill so progress does not depend on one interrupt edge.

## STREAM_CTRL Bits

| Bit | Name      | Description |
|-----|-----------|-------------|
| 0   | MODE      | `0` = legacy fixed preload, `1` = stream FIFO mode |
| 1   | OVERFLOW  | Stream push overflow sticky flag; write 1 to clear |
| 2   | EOF_SEEN  | Read-only sticky indication that an EOF event fired |
| 3   | DMA_MODE  | `1` selects the scheduler AXI-Stream DMA ingress |

`STREAM_CTRL.MODE` and `DMA_MODE` are captured at ARM and locked for the run.
Firmware must stop or reset before changing them.

`CTRL.RESET_SOFT` flushes the stream FIFO, clears stream counters/sticky flags,
and is the required stream recovery path.

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

Defined event flags:

| Bit | Name |
|-----|------|
| 0   | PHASE_REINIT |
| 1   | EOF |

EOF is meaningful in stream mode. The EOF event is applied normally; after it
fires and is popped, the engine transitions to DONE and sets `EOF_SEEN`.

## Event Write Sequence

Legacy preload sequence:

1. Write `EVT_WADDR = <index>`.
2. Write `EVT_WDATA0..6`.
3. Write `EVT_WCTRL = 1`.
4. Write `EVENT_COUNT`.
5. Arm and run with `STREAM_CTRL.MODE = 0`.

Stream sequence:

1. Set `STREAM_CTRL.MODE = 1` before ARM.
2. Check `FREE_SPACE`.
3. Write `EVT_WDATA0..6`.
4. Write `EVT_WCTRL = 1`.
5. Check `STREAM_CTRL.OVERFLOW`; if set, the event was refused.

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
2. read `IP_VERSION`; fail with `-ENOTSUP` if not `0x00010000`
3. read `IP_CAPS`; store event depth / payload bits / timestamp bits

That lets firmware detect a mismatched or absent bitstream before touching the
rest of the block.

`awg_sched_stream_open()` repeats the `IP_ID` / `IP_VERSION` check and reads
`STREAM_DEPTH`, because stream FIFO depth is not derived from legacy
`IP_CAPS`.

## tick_hz

`awg_sched_cfg_t.tick_hz` is informational only. The current contract does not
expose tick rate through `IP_CAPS`, so firmware and host must agree on it
explicitly.

For the current KCU116 AWG image the working value is `245760000`, matching
the scheduler clock domain. Older captures and logs may show `100000000`; do
not use that stale value for new host-generated event timestamps.

## Optional Load Readback Verification

Controlled by `AWG_SCHED_ENABLE_LOAD_READBACK_VERIFY` (default: 0).

This must stay disabled for the current HDL. Reads from `EVT_WDATA0..6` return
the last staged write registers, not indexed event-memory contents, so naive
post-load compare logic produces false mismatches after the first event.

## Host Upload Transport

The UART preload console accepts `LOADBIN <count>` followed by packed events
encoded as ASCII hex, not raw binary.

Expected console sequence:

1. `[AWG-UART] LOADBIN READY ...`
2. `[AWG-UART] LOADBIN RX BEGIN bytes=<n> hex_chars=<2n>`
3. `[AWG-UART] LOADBIN RX OK ...`
4. `[AWG-UART] LOADBIN COMMIT BEGIN ...`
5. `[AWG-UART] LOADBIN OK ...`

`STREAMHEX <byte_count>` carries the legacy GWAS/1 frame as ASCII hex. GWAS/1
is a diagnostic compatibility protocol:

```text
u32 magic      // 0x53415747
u32 seq
u16 n_events
u16 flags      // bit0=open, bit1=close_with_eof
awg_event_v1_t events[n_events]
u32 crc32_ieee
```

Its ACK format is:

```text
u32 magic
u32 seq_acked
u32 ddr_free_events
u32 status
u32 stream_free_events
u32 stream_stalls
u32 irq_status
```

The UART response includes those fields plus firmware return and frame
metadata. UARTLite at 115200 baud is for correctness checks, not high-rate
streaming.

GWAS/2 is the production UDP protocol. Each frame contains this 24-byte
little-endian header, zero or more 32-byte records, and IEEE CRC32:

```text
u32 magic            // 0x53415747
u8  version          // 2
u8  kind             // 0=CONTROL, 1=direct events, 2=C1
u16 flags            // OPEN or CLOSE_WITH_EOF
u32 session_id
u32 sequence
u16 record_count
u16 header_bytes     // 24
u32 payload_bytes
u8  records[record_count][32]
u32 crc32_ieee
```

Session order is strict:

1. CONTROL OPEN at sequence 0 with one 32-byte SHA-256 program record.
2. Direct-event or C1 frames at consecutive sequence numbers.
3. Zero-record CONTROL CLOSE. Direct mode uses `CLOSE_WITH_EOF`; C1 mode does
   not add a firmware EOF event.

The 40-byte ACK returns magic, version, session, acknowledged sequence, status,
ring free records, scheduler status, hardware stream free records, stream
stalls, and IRQ state. A retry must repeat the identical datagram. See
`awg_stream_sender_v2.py` and `tests/awg_stream_proto_v2_test.c`.

At IPv4 MTU 1500, one GWAS/2 data frame can carry at most 45 records without
fragmentation. Firmware rejects IPv4 fragments. Larger batches require a
matching jumbo MTU and must still stay at or below 128 records.

### C1 records

A C1 program is a sequence of 32-byte little-endian records:

1. Header 0: magic `0x43475741`, version 1, header size 64, record size 32,
   flags, start timestamp, command count, and declared repeat depth.
2. Header 1: declared output-event count, command bytes, input CRC64, and a
   zero reserved word.
3. The declared command records.

The input CRC uses polynomial `0x42F0E1EBA9EA3693`, initial value zero, no
reflection, and no final XOR. Calculate it over Header 0, Header 1 with its CRC
field zero, and every command record. Header flag bit 0 makes the decoder add
EOF to the final output event.

The decoder supports `WAIT`, `FIRE`, `LINEAR`, `LINEAR_CONT`,
`REPEAT_BEGIN`, and `REPEAT_END`. Use `awg_c1_program.py` to create a finite
`LINEAR` + `LINEAR_CONT` program; use the RTL in
`hdl-adi-fork/projects/awg/common/awg_extension.v` as the full field-level
source of truth.

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

Read legacy capacity from `IP_CAPS`; do not assume the value from an older
bitstream. Read streaming capacity from `STREAM_DEPTH`. The current default
FIFO has 512 physical slots and reports 511 usable events.

Sequences larger than legacy preload capacity must use host batching, software
streaming, scheduler DMA, or the production Ethernet path. `FREE_SPACE` is the
hardware truth for each refill. A source or host test result is not evidence of
the sustained hardware event rate; use the closure procedure in
`../BUILD_AND_USE.md`.

## Artifact Dump

`awg_sched_dump_artifacts(events, count, status)` emits machine-parsable lines
prefixed `[SCHED-ARTIFACT]`:

```text
[SCHED-ARTIFACT] config base=0x44AA0000 max_events=64 tick_hz=245760000 timeout_ms=2000
[SCHED-ARTIFACT] event idx=0 ts=0x00000000_000003E8 ch=0 fl=0x0001 p0=0x00010000 p1=... p2=... p3=...
[SCHED-ARTIFACT] status armed=1 running=0 done=1 error=0 err_code=0x00 current=4 loaded=4 commit=4 reinit=0 reinit_reject=0 irq=0x00000001
[SCHED-ARTIFACT] time_now=0x00000000_00001388 last_exec=0x00000000_00000FA0
[SCHED-ARTIFACT] stream depth=511 low_wmark=127 ctrl=0x00000005 occupancy=0 free_space=511 pushes=4 stalls=0 irq=0x00000001 err=0x00000000
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

Build every target profile from its real XSA on the build machine. For example:

```powershell
make -C projects\fmcdac reset FMCDAC_AWG_PROFILE=scheduler-eth
make -C projects\fmcdac FMCDAC_AWG_PROFILE=scheduler-eth
```
