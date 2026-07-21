#!/usr/bin/env python3
"""Generate one finite C1 LINEAR program for the AWG extension decoder."""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path
from typing import Sequence


MAGIC = 0x43475741
VERSION = 1
HEADER_BYTES = 64
RECORD_BYTES = 32
OP_LINEAR = 0x03
OP_LINEAR_CONT = 0x04
HEADER_FLAG_EOF = 1 << 0
EVENT_FLAG_PHASE_REINIT = 1 << 0
CRC64_POLY = 0x42F0E1EBA9EA3693
U64_MASK = 0xFFFFFFFFFFFFFFFF

HEADER0 = struct.Struct("<IHHHHIQII")
HEADER1 = struct.Struct("<QQQQ")
LINEAR = struct.Struct("<BBHHHIIHHIII")
LINEAR_CONT = struct.Struct("<B3xqqqI")


def auto_int(text: str) -> int:
    return int(text, 0)


def crc64_awg(records: bytes) -> int:
    """Match awg_extension.v crc64_word, byte for byte."""

    crc = 0
    for value in records:
        crc ^= value << 56
        for _ in range(8):
            if crc & (1 << 63):
                crc = ((crc << 1) & U64_MASK) ^ CRC64_POLY
            else:
                crc = (crc << 1) & U64_MASK
    return crc


def checked_linear_end(start: int, step: int, count: int, limit: int, name: str) -> None:
    end = start + (count - 1) * step
    if not 0 <= start <= limit or not 0 <= end <= limit:
        raise ValueError(f"{name} leaves the range 0..0x{limit:X}")


def build_program(args: argparse.Namespace) -> tuple[bytes, dict]:
    if not 1 <= args.count <= 0xFFFFFFFF:
        raise ValueError("count must be in the range 1..0xFFFFFFFF")
    if not 0 <= args.start_ticks <= U64_MASK:
        raise ValueError("start-ticks must fit in 64 bits")
    if not 1 <= args.dwell_ticks <= 0xFFFFFFFF:
        raise ValueError("dwell-ticks must be in the range 1..0xFFFFFFFF")
    if args.start_ticks + args.count * args.dwell_ticks > U64_MASK:
        raise ValueError("the C1 schedule advances past the 64-bit tick range")
    if not 0 <= args.channel <= 0xFFFF:
        raise ValueError("channel must fit in 16 bits")
    for value, name in (
        (args.scale_step, "scale-step"),
        (args.initial_phase_step, "initial-phase-step"),
        (args.phase_increment_step, "phase-increment-step"),
    ):
        if not -(1 << 63) <= value < (1 << 63):
            raise ValueError(f"{name} must fit in a signed 64-bit value")

    checked_linear_end(args.scale, args.scale_step, args.count, 0xFFFF, "scale")
    checked_linear_end(
        args.initial_phase,
        args.initial_phase_step,
        args.count,
        0xFFFFFFFF,
        "initial phase",
    )
    checked_linear_end(
        args.phase_increment,
        args.phase_increment_step,
        args.count,
        0xFFFFFFFF,
        "phase increment",
    )

    event_flags = EVENT_FLAG_PHASE_REINIT if args.phase_reinit else 0
    header_flags = 0 if args.no_eof else HEADER_FLAG_EOF
    command_count = 2
    header0 = HEADER0.pack(
        MAGIC,
        VERSION,
        0,
        HEADER_BYTES,
        RECORD_BYTES,
        header_flags,
        args.start_ticks,
        command_count,
        0,
    )
    linear = LINEAR.pack(
        OP_LINEAR,
        0,
        args.channel,
        event_flags,
        0,
        args.count,
        args.dwell_ticks,
        args.scale,
        0,
        args.initial_phase,
        args.phase_increment,
        0,
    )
    continuation = LINEAR_CONT.pack(
        OP_LINEAR_CONT,
        args.scale_step,
        args.initial_phase_step,
        args.phase_increment_step,
        0,
    )

    header1_without_crc = HEADER1.pack(
        args.count,
        command_count * RECORD_BYTES,
        0,
        0,
    )
    crc = crc64_awg(header0 + header1_without_crc + linear + continuation)
    header1 = HEADER1.pack(
        args.count,
        command_count * RECORD_BYTES,
        crc,
        0,
    )
    program = header0 + header1 + linear + continuation
    summary = {
        "format": "AWG C1 v1",
        "records": len(program) // RECORD_BYTES,
        "bytes": len(program),
        "commands": command_count,
        "declared_events": args.count,
        "start_ticks": args.start_ticks,
        "dwell_ticks": args.dwell_ticks,
        "eof_on_last_event": not args.no_eof,
        "input_crc64": f"0x{crc:016X}",
    }
    return program, summary


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--metadata", type=Path)
    parser.add_argument("--count", type=auto_int, default=1024)
    parser.add_argument("--start-ticks", type=auto_int, default=61_440_000)
    parser.add_argument("--dwell-ticks", type=auto_int, default=24_576)
    parser.add_argument("--channel", type=auto_int, default=0)
    parser.add_argument("--scale", type=auto_int, default=0)
    parser.add_argument("--initial-phase", type=auto_int, default=0)
    parser.add_argument("--phase-increment", type=auto_int, default=0)
    parser.add_argument("--scale-step", type=auto_int, default=0)
    parser.add_argument("--initial-phase-step", type=auto_int, default=0)
    parser.add_argument("--phase-increment-step", type=auto_int, default=0)
    parser.add_argument("--phase-reinit", action="store_true")
    parser.add_argument(
        "--no-eof",
        action="store_true",
        help="do not mark the last decoded event EOF",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        program, summary = build_program(args)
        output = args.output.expanduser().resolve()
        if output.exists():
            raise FileExistsError(f"refusing to overwrite: {output}")
        if args.metadata:
            metadata = args.metadata.expanduser().resolve()
            if metadata.exists():
                raise FileExistsError(f"refusing to overwrite: {metadata}")
        else:
            metadata = None

        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(program)
        summary["output"] = str(output)
        if metadata:
            summary["metadata"] = str(metadata)
            metadata.parent.mkdir(parents=True, exist_ok=True)
            metadata.write_text(
                json.dumps(summary, indent=2) + "\n",
                encoding="utf-8",
            )
    except (OSError, ValueError) as exc:
        print(f"awg_c1_program: {exc}", file=sys.stderr)
        return 2

    print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
