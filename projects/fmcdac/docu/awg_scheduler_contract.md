# AWG Scheduler Contract (v2)

This document is the software↔HDL contract for the AWG timed-control peripheral
used by `projects/fmcdac/src/app/awg_sched.{h,c}`.

All registers are 32-bit, little-endian at the CPU AXI4-Lite interface.
All offsets are byte addresses relative to the peripheral base address.

The register constants live in `awg_sched_regs.h`.  When a register-generation
toolflow is adopted that header must be regenerated from the same YAML source
as the HDL localparams.

---

## Register Map

| Offset | Name            | Access | Description                                |
|--------|-----------------|--------|--------------------------------------------|
| 0x00   | CTRL            | W      | Control strobe register                    |
| 0x04   | STATUS          | R      | Status and error-code register             |
| 0x08   | EVT_COUNT       | RW     | Number of loaded events                    |
| 0x0C   | CUR_EVT         | R      | Index of currently executing event         |
| 0x10   | ERR_REG         | R      | Latched hardware error code                |
| 0x14   | IP_ID           | R      | IP magic word (0x41574753 = `'AWGS'`)      |
| 0x18   | IP_VERSION      | R      | Major\[31:16\] / Minor\[15:0\]             |
| 0x1C   | IP_CAPS         | R      | Capability word (see below)                |
| 0x20   | TIME_LO         | R      | Tick counter \[31:0\] (read after STATUS to get a consistent snapshot) |
| 0x24   | TIME_HI         | R      | Tick counter \[63:32\] (read after TIME_LO for coherent pair)          |
| 0x28   | LAST_EXEC_LO    | R      | Last-dispatched event tick \[31:0\]        |
| 0x2C   | LAST_EXEC_HI    | R      | Last-dispatched event tick \[63:32\]       |
| 0x30   | COMMIT_COUNT    | R      | Total events dispatched since reset        |
| 0x34   | REINIT_COUNT    | R      | Sequence re-start count since reset        |
| 0x38   | REINIT_REJECT   | R      | Re-init requests rejected (timing)         |
| 0x3C   | IRQ_STATUS      | R/W1C  | Latched interrupt flags                    |
| 0x40   | EVT_WADDR       | W      | Event BRAM write address                   |
| 0x44   | EVT_WDATA0      | W      | Timestamp \[31:0\]                         |
| 0x48   | EVT_WDATA1      | W      | Timestamp \[63:32\]                        |
| 0x4C   | EVT_WDATA2      | W      | Channel\[31:16\] \| Flags\[15:0\]          |
| 0x50   | EVT_WDATA3      | W      | Payload word 0                             |
| 0x54   | EVT_WDATA4      | W      | Payload word 1                             |
| 0x58   | EVT_WDATA5      | W      | Payload word 2                             |
| 0x5C   | EVT_WDATA6      | W      | Payload word 3                             |
| 0x60   | EVT_WCTRL       | W      | Write 1 to commit event data to BRAM       |
| 0x74   | IRQ_ENABLE      | RW     | Interrupt enable mask (bit0=done, bit1=error) |
| 0x78   | TIME_RELOAD_LO  | W      | Epoch reload value low word                |
| 0x7C   | TIME_RELOAD_HI  | W      | Epoch reload value high word               |
| 0x80   | TIME_RELOAD_CTRL| W      | Write 1 to reload on next SYSREF           |

---

## CTRL Register Bit Fields

| Bit | Name         | Description                                         |
|-----|--------------|-----------------------------------------------------|
| 0   | RUN          | Assert to start sequence execution                  |
| 1   | ARM          | Arm hardware trigger gate                           |
| 2   | STOP_REQ     | Request graceful stop after current event           |
| 3   | RESET_SOFT   | Pulse to clear state (does NOT fire commit pulse)   |

> **Note**: CTRL\[0\]=1 was previously mis-documented as soft-reset.  It is
> `RUN`.  Soft-reset is on bit 3.  Using the wrong bit would fire a
> `marker_commit` pulse on the scope pin instead of resetting state.
>
> Firmware emits ARM and RUN as **two separate AXI writes** (with a status
> round-trip in between) to avoid a simultaneous-edge race in implementations
> that sample both request toggles in the same scheduler clock cycle.

---

## STATUS Register Bit Fields

| Bits  | Name      | Description                                          |
|-------|-----------|------------------------------------------------------|
| 0     | armed     | Hardware trigger gate is armed                       |
| 1     | running   | Sequence is currently executing                      |
| 2     | done      | Sequence has completed                               |
| 3     | error     | Hardware error latched; see ERR_REG                  |
| 15:8  | err_code  | Latched error code (mirrors ERR_REG\[7:0\])          |

---

## IP_CAPS Register

| Bits  | Field          | Description                                     |
|-------|----------------|-------------------------------------------------|
| 31:24 | evt_depth_log2 | `1 << field` gives maximum event BRAM depth     |
| 23:16 | payload_bits   | Payload width in bits                           |
| 15:8  | ts_bits        | Timestamp width in bits                         |
| 7:0   | reserved       | Must be zero                                    |

---

## IRQ_STATUS Register Bit Fields

| Bit | Name             | Description               |
|-----|------------------|---------------------------|
| 0   | done_irq         | Sequence-done interrupt   |
| 1   | error_irq        | Error interrupt           |
| 2   | spacing_viol_irq | Timing-violation interrupt|

---

## Event Format (v1, 256 bits = 32 bytes)

Event struct `awg_event_v1_t`:

| Byte offset | Field            | Type      | Description                  |
|-------------|------------------|-----------|------------------------------|
| 0           | timestamp_ticks  | uint64_t  | 64-bit tick timestamp        |
| 8           | channel          | uint16_t  | Output channel selector      |
| 10          | flags            | uint16_t  | Event-type flags             |
| 12          | payload          | 4×uint32  | DDS control words (16 bytes) |
| 28          | reserved         | uint32_t  | Must be zero                 |

Compile-time ABI guards enforce these offsets and `sizeof(awg_event_v1_t)==32`.

---

## Event Write Sequence

1. Write `EVT_WADDR = <index>`.
2. Write `EVT_WDATA0..6` (order does not matter before the strobe).
3. Write `EVT_WCTRL = 1` to commit.

The 7-word mapping is:

| Register  | Content                            |
|-----------|------------------------------------|
| EVT_WDATA0 | `timestamp_ticks[31:0]`           |
| EVT_WDATA1 | `timestamp_ticks[63:32]`          |
| EVT_WDATA2 | `channel[31:16] \| flags[15:0]`   |
| EVT_WDATA3 | `payload.word0`                   |
| EVT_WDATA4 | `payload.word1`                   |
| EVT_WDATA5 | `payload.word2`                   |
| EVT_WDATA6 | `payload.word3`                   |

---

## Payload v1 Bit Contract

`payload.word0`:
- `[15:0]`  `tone`
- `[31:16]` `freq_lsb16`

`payload.word1`:
- `[15:0]`  `scale`
- `[31:16]` `reserved0` (must be zero unless reused by a newer contract)

`payload.word2`:
- `[15:0]`  `phase`
- `[31:16]` `reserved1` (must be zero unless reused by a newer contract)

`payload.word3`:
- `[31:0]`  `user_word3` (opaque extension word)

---

## IP Identity Handshake

`awg_sched_config()` performs an IP identity check before issuing soft-reset:

1. Read `IP_ID`; fail with `-ENODEV` if `!= 0x41574753` (`'AWGS'`).
2. Read `IP_VERSION`; fail with `-ENOTSUP` if major version does not match
   `AWG_TIMED_CTRL_MAJOR_EXPECTED` (currently `1`).
3. Read `IP_CAPS`; store `hw_event_depth`, `hw_payload_bits`, `hw_ts_bits`
   for runtime capability queries.

This ensures firmware detects a mismatched or absent bitstream before
touching any other register.

---

## tick_hz

`awg_sched_cfg_t.tick_hz` is **informational only**.  The actual tick rate
is a static HDL build parameter readable via `IP_CAPS`.  It is not written
to any hardware register.

---

## Optional Load Readback Verification

Controlled by `AWG_SCHED_ENABLE_LOAD_READBACK_VERIFY` (default: 1).

After writing all events, `awg_sched_load_events()` re-selects each
`EVT_WADDR` and reads back `EVT_WDATA0..6`, comparing against the
expected values.  Any mismatch returns `-EIO` and logs a detailed
expected-vs-actual line to help diagnose packing or addressing bugs.

---

## Artifact Dump

`awg_sched_dump_artifacts(events, count, status)` emits machine-parsable
lines prefixed `[SCHED-ARTIFACT]` that can be grepped by a host-side parser:

```
[SCHED-ARTIFACT] config base=0x44AA0000 max_events=64 tick_hz=1000000 timeout_ms=2000
[SCHED-ARTIFACT] event idx=0 ts=0x00000000_000003E8 ch=0 fl=0x0001 p0=0x00010000 p1=... p2=... p3=...
[SCHED-ARTIFACT] status armed=1 running=0 done=1 error=0 err_code=0x00 current=4 loaded=4 commit=4 reinit=0 reinit_reject=0 irq=0x00000001
[SCHED-ARTIFACT] time_now=0x00000000_00001388 last_exec=0x00000000_00000FA0
```

---

## Host-Side Unit Tests

`projects/fmcdac/tests/` contains a host-buildable test suite covering
every `AWG_EVTVAL_ERR_*` path.  Build and run with:

```sh
make -C projects/fmcdac/tests run
```

The test binary links `awg_sched.c` against stub implementations of
`no_os_axi_io_*` and `no_os_mdelay`.  IP identity registers are pre-populated
in the stub register bank so `awg_sched_config()` succeeds without hardware.
