# AWG Scheduler Event Packing Contract (v1)

This document is the software↔HDL contract for the AWG scheduler event memory/register format used by:

- `projects/fmcdac/src/app/awg_sched.h`
- `projects/fmcdac/src/app/awg_sched.c`

All words are 32-bit and little-endian at the CPU interface.

## Event layout (6 words per event)

Per event index `i`, software writes `EVENT_ADDR = i`, then the following register words:

| Sequential word | Register | Bits | Field |
|---|---|---|---|
| word0 | `EVENT_TIME` | `[31:0]` | `timestamp_ticks` |
| word1 | `EVENT_CH_FLAGS` | `[31:16]` | `channel` |
|  |  | `[15:0]` | `flags` |
| word2 | `EVENT_PAYLOAD0` | `[31:0]` | `payload.word0` |
| word3 | `EVENT_PAYLOAD1` | `[31:0]` | `payload.word1` |
| word4 | `EVENT_PAYLOAD2` | `[31:0]` | `payload.word2` |
| word5 | `EVENT_PAYLOAD3` | `[31:0]` | `payload.word3` |

## Payload v1 bit contract

`payload.word0`:
- `[15:0]`  `tone`
- `[31:16]` `freq_lsb16`

`payload.word1`:
- `[15:0]`  `scale`
- `[31:16]` `reserved0` (must be written as 0 unless reused by a newer contract)

`payload.word2`:
- `[15:0]`  `phase`
- `[31:16]` `reserved1` (must be written as 0 unless reused by a newer contract)

`payload.word3`:
- `[31:0]`  `user_word3` (opaque extension word)

## Compile-time ABI guards in code

The C header enforces the ABI with compile-time checks:

- `_Static_assert(sizeof(awg_payload_v1_t) == 16)`
- `_Static_assert(sizeof(awg_event_v1_t) == 24)`
- `offsetof(...)` checks for word and field positions

These checks intentionally fail the build if packing/alignment changes.

## Optional load readback verification

`awg_sched_verify_events()` supports optional readback verification (enabled by default via
`AWG_SCHED_ENABLE_LOAD_READBACK_VERIFY` in `awg_sched.c`).

After events are written, software can re-select each `EVENT_ADDR` and read back:

- `EVENT_TIME`
- `EVENT_CH_FLAGS`
- `EVENT_PAYLOAD0..3`

Any mismatch returns an error early (`-EIO`) and prints a detailed expected-vs-actual line,
which helps detect:

- software packing mistakes,
- endianness or field-order mistakes,
- scheduler BRAM addressing mismatches.
