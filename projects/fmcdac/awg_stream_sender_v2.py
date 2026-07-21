#!/usr/bin/env python3
"""Send production GWAS/2 AWG programs to the FMCDAC Phase-F firmware."""

from __future__ import annotations

import argparse
import hashlib
import ipaddress
import json
import math
import secrets
import socket
import struct
import sys
import time
import zlib
from datetime import datetime, timezone
from pathlib import Path
from typing import Sequence


MAGIC = 0x53415747
VERSION = 2

KIND_CONTROL = 0
KIND_EVENTS = 1
KIND_C1 = 2
KIND_NAMES = {
    KIND_CONTROL: "control",
    KIND_EVENTS: "direct",
    KIND_C1: "c1",
}

FLAG_OPEN = 1 << 0
FLAG_CLOSE_WITH_EOF = 1 << 1

EVENT_FLAG_PHASE_REINIT = 1 << 0
EVENT_FLAG_EOF = 1 << 1

HEADER = struct.Struct("<IBBHIIHHI")
ACK = struct.Struct("<IBBHIIIIIIII")
EVENT = struct.Struct("<QHHIIIII")
CRC = struct.Struct("<I")

HEADER_BYTES = 24
ACK_BYTES = 40
RECORD_BYTES = 32
MAX_FRAME_RECORDS = 128
DEFAULT_IPV4_MTU = 1500
IPV4_HEADER_BYTES = 20
UDP_HEADER_BYTES = 8

ACK_STATUS = {
    1 << 0: "BAD_ARG",
    1 << 1: "BAD_MAGIC",
    1 << 2: "BAD_LENGTH",
    1 << 3: "BAD_CRC",
    1 << 4: "DISABLED",
    1 << 5: "OPEN_FAILED",
    1 << 6: "RING_FULL",
    1 << 7: "OVERFLOW",
    1 << 8: "SCHED_ERROR",
    1 << 9: "CLOSE_FAILED",
    1 << 10: "BAD_SEQUENCE",
    1 << 11: "BAD_FLAGS",
    1 << 12: "BAD_SESSION",
    1 << 13: "DMA_ERROR",
    1 << 14: "BAD_EVENT",
    1 << 15: "BAD_VERSION",
    1 << 16: "BAD_KIND",
    1 << 17: "C1_DISABLED",
}
ACK_STATUS_KNOWN_MASK = sum(ACK_STATUS)
ACK_STATUS_RING_FULL = 1 << 6


class ProtocolError(RuntimeError):
    """A received datagram does not match the GWAS/2 ACK ABI."""


class ExchangeError(RuntimeError):
    """A logical frame was not accepted after its allowed attempts."""

    def __init__(self, message: str, row: dict) -> None:
        super().__init__(message)
        self.row = row


def _auto_int(text: str) -> int:
    return int(text, 0)


def _status_names(status: int) -> list[str]:
    if status == 0:
        return ["OK"]
    names = [name for bit, name in ACK_STATUS.items() if status & bit]
    unknown = status & ~ACK_STATUS_KNOWN_MASK
    if unknown:
        names.append(f"UNKNOWN_0x{unknown:08X}")
    return names


def _status_text(status: int) -> str:
    return "|".join(_status_names(status))


def _max_records_for_mtu(mtu: int) -> int:
    payload_bytes = (
        mtu
        - IPV4_HEADER_BYTES
        - UDP_HEADER_BYTES
        - HEADER_BYTES
        - CRC.size
    )
    return min(MAX_FRAME_RECORDS, payload_bytes // RECORD_BYTES)


def _build_frame(
    session_id: int,
    seq: int,
    kind: int,
    flags: int,
    records: bytes,
) -> bytes:
    if len(records) % RECORD_BYTES:
        raise ValueError("frame records must be a multiple of 32 bytes")
    count = len(records) // RECORD_BYTES
    if count > MAX_FRAME_RECORDS:
        raise ValueError("a GWAS/2 frame may contain at most 128 records")

    header = HEADER.pack(
        MAGIC,
        VERSION,
        kind,
        flags,
        session_id,
        seq,
        count,
        HEADER_BYTES,
        len(records),
    )
    body = header + records
    return body + CRC.pack(zlib.crc32(body) & 0xFFFFFFFF)


def _parse_ack(data: bytes) -> dict:
    if len(data) != ACK_BYTES:
        raise ProtocolError(f"ACK length is {len(data)}, expected {ACK_BYTES}")

    (
        magic,
        version,
        reserved,
        header_bytes,
        session_id,
        seq_acked,
        status,
        free_records,
        scheduler_status,
        stream_free_records,
        stream_stalls,
        irq_status,
    ) = ACK.unpack(data)

    if magic != MAGIC:
        raise ProtocolError(f"ACK magic is 0x{magic:08X}, expected 0x{MAGIC:08X}")
    if version != VERSION:
        raise ProtocolError(f"ACK version is {version}, expected {VERSION}")
    if reserved != 0:
        raise ProtocolError(f"ACK reserved byte is nonzero: 0x{reserved:02X}")
    if header_bytes != ACK_BYTES:
        raise ProtocolError(
            f"ACK header size is {header_bytes}, expected {ACK_BYTES}"
        )

    return {
        "session_id": session_id,
        "seq_acked": seq_acked,
        "status": status,
        "status_names": _status_names(status),
        "free_records": free_records,
        "scheduler_status": scheduler_status,
        "stream_free_records": stream_free_records,
        "stream_stalls": stream_stalls,
        "irq_status": irq_status,
    }


def _exchange(
    sock: socket.socket,
    frame: bytes,
    *,
    session_id: int,
    seq: int,
    phase: str,
    kind: int,
    record_count: int,
    ack_timeout: float,
    retries: int,
) -> dict:
    row = {
        "phase": phase,
        "seq": seq,
        "kind": KIND_NAMES[kind],
        "records": record_count,
        "frame_bytes": len(frame),
        "frame_crc32": f"0x{CRC.unpack_from(frame, len(frame) - CRC.size)[0]:08X}",
        "attempts": 0,
        "responses": [],
    }
    last_problem = "ACK timeout"

    for attempt in range(1, retries + 2):
        row["attempts"] = attempt
        sent_at = time.monotonic()
        sock.send(frame)
        deadline = sent_at + ack_timeout
        retry_frame = False

        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            sock.settimeout(remaining)
            try:
                data = sock.recv(65535)
            except socket.timeout:
                break

            received_at = time.monotonic()
            try:
                ack = _parse_ack(data)
            except ProtocolError as exc:
                last_problem = str(exc)
                row["responses"].append(
                    {
                        "attempt": attempt,
                        "ignored": last_problem,
                        "datagram_bytes": len(data),
                    }
                )
                continue

            ack["attempt"] = attempt
            ack["round_trip_s"] = received_at - sent_at
            if ack["session_id"] != session_id or ack["seq_acked"] != seq:
                last_problem = (
                    "unmatched ACK "
                    f"session=0x{ack['session_id']:08X} seq={ack['seq_acked']}"
                )
                ack["ignored"] = last_problem
                row["responses"].append(ack)
                continue

            row["responses"].append(ack)
            row["ack"] = ack
            row["service_s"] = received_at - sent_at
            if ack["status"] == 0:
                return row

            last_problem = f"ACK status {_status_text(ack['status'])}"
            if ack["status"] == ACK_STATUS_RING_FULL and attempt <= retries:
                retry_frame = True
                break
            raise ExchangeError(last_problem, row)

        if attempt <= retries:
            if retry_frame:
                time.sleep(min(0.010, ack_timeout))
            continue

    row["error"] = last_problem
    raise ExchangeError(
        f"{phase} sequence {seq} failed after {retries + 1} attempts: "
        f"{last_problem}",
        row,
    )


def _read_records(path: Path) -> bytes:
    data = path.read_bytes()
    if not data:
        raise ValueError(f"record file is empty: {path}")
    if len(data) % RECORD_BYTES:
        raise ValueError(
            f"record file size {len(data)} is not a multiple of {RECORD_BYTES}"
        )
    return data


def _validate_direct_records(records: bytes) -> None:
    previous_timestamp: int | None = None
    for index in range(0, len(records), RECORD_BYTES):
        event = EVENT.unpack_from(records, index)
        timestamp = event[0]
        flags = event[2]
        reserved = event[-1]
        record_number = index // RECORD_BYTES

        if previous_timestamp is not None and timestamp < previous_timestamp:
            raise ValueError(
                f"direct record {record_number} timestamp moves backward"
            )
        if flags & EVENT_FLAG_EOF:
            raise ValueError(
                f"direct record {record_number} contains EOF; CONTROL CLOSE owns EOF"
            )
        if flags & ~EVENT_FLAG_PHASE_REINIT:
            raise ValueError(
                f"direct record {record_number} has unsupported flags 0x{flags:04X}"
            )
        if reserved != 0:
            raise ValueError(f"direct record {record_number} reserved word is nonzero")
        previous_timestamp = timestamp


def _dds_payload(scale: int, initial_phase: int, phase_increment: int) -> bytes:
    payload = (
        scale
        | (initial_phase << 16)
        | (phase_increment << 48)
    )
    return payload.to_bytes(16, "little")


def _generate_direct_records(args: argparse.Namespace) -> bytes:
    if args.count <= 0:
        raise ValueError("count must be positive")
    if args.tick_step <= 0:
        raise ValueError("tick-step must be positive")
    start_ticks = args.start_ticks
    if start_ticks is None:
        upload_s = args.count / args.rate if args.rate > 0 else 0.0
        start_ticks = math.ceil(
            (args.startup_margin + upload_s) * args.tick_hz
        )
        args.start_ticks = start_ticks
    if not 0 <= start_ticks <= 0xFFFFFFFFFFFFFFFF:
        raise ValueError("start-ticks must fit in 64 bits")
    if not 0 <= args.channel <= 0xFFFF:
        raise ValueError("channel must fit in 16 bits")
    if not 0 <= args.scale <= 0xFFFF:
        raise ValueError("scale must fit in 16 bits")
    if not 0 <= args.initial_phase <= 0xFFFFFFFF:
        raise ValueError("initial-phase must fit in 32 bits")
    if not 0 <= args.phase_increment <= 0xFFFFFFFF:
        raise ValueError("phase-increment must fit in 32 bits")
    if not 0 <= args.phase_increment_step <= 0xFFFFFFFF:
        raise ValueError("phase-increment-step must fit in 32 bits")

    flags = EVENT_FLAG_PHASE_REINIT if args.phase_reinit else 0
    records = bytearray()
    for index in range(args.count):
        timestamp = start_ticks + index * args.tick_step
        if timestamp > 0xFFFFFFFFFFFFFFFF:
            raise ValueError(f"timestamp for direct record {index} exceeds 64 bits")
        increment = (
            args.phase_increment + index * args.phase_increment_step
        ) & 0xFFFFFFFF
        payload = _dds_payload(args.scale, args.initial_phase, increment)
        payload_words = struct.unpack("<IIII", payload)
        records.extend(
            EVENT.pack(
                timestamp,
                args.channel,
                flags,
                *payload_words,
                0,
            )
        )
    return bytes(records)


def _write_telemetry(path: Path, telemetry: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(telemetry, indent=2) + "\n", encoding="utf-8")


def _send_program(args: argparse.Namespace, telemetry: dict) -> dict:
    ipaddress.IPv4Address(args.dest_ip)
    if args.source_ip:
        ipaddress.IPv4Address(args.source_ip)
    if not 1 <= args.port <= 65535:
        raise ValueError("port must be in the range 1..65535")
    if not 0 <= args.source_port <= 65535:
        raise ValueError("source-port must be in the range 0..65535")
    if not 1 <= args.batch <= MAX_FRAME_RECORDS:
        raise ValueError("batch must be in the range 1..128")
    if not 68 <= args.mtu <= 65535:
        raise ValueError("mtu must be in the range 68..65535")
    mtu_records = _max_records_for_mtu(args.mtu)
    if mtu_records < 1:
        raise ValueError(
            f"mtu {args.mtu} is too small for one GWAS/2 record"
        )
    if args.batch > mtu_records:
        raise ValueError(
            f"batch {args.batch} exceeds the unfragmented IPv4 limit "
            f"of {mtu_records} records for MTU {args.mtu}"
        )
    if args.rate < 0:
        raise ValueError("rate must not be negative")
    if args.tick_hz <= 0:
        raise ValueError("tick-hz must be positive")
    if args.startup_margin < 0:
        raise ValueError("startup-margin must not be negative")
    if args.ack_timeout <= 0:
        raise ValueError("ack-timeout must be positive")
    if args.retries < 0:
        raise ValueError("retries must not be negative")

    payload_kind = KIND_EVENTS if args.kind == "direct" else KIND_C1
    if args.kind == "c1" and args.records_file is None:
        raise ValueError("--kind c1 requires --records-file")

    if args.records_file is not None:
        records = _read_records(args.records_file)
    else:
        records = _generate_direct_records(args)
    if payload_kind == KIND_EVENTS:
        _validate_direct_records(records)

    record_count = len(records) // RECORD_BYTES
    program_digest = hashlib.sha256(records).digest()
    session_id = (
        args.session_id
        if args.session_id is not None
        else secrets.randbits(32)
    )
    if not 0 <= session_id <= 0xFFFFFFFF:
        raise ValueError("session-id must fit in 32 bits")

    telemetry.update(
        {
            "protocol": "GWAS/2",
            "destination": f"{args.dest_ip}:{args.port}",
            "source": (
                f"{args.source_ip}:{args.source_port}"
                if args.source_ip
                else "system-selected"
            ),
            "session_id": f"0x{session_id:08X}",
            "payload_kind": args.kind,
            "program_records": record_count,
            "program_bytes": len(records),
            "program_sha256": program_digest.hex(),
            "batch": args.batch,
            "ipv4_mtu": args.mtu,
            "max_unfragmented_batch": mtu_records,
            "target_records_per_s": args.rate,
        }
    )
    if args.records_file is None:
        telemetry["generated_direct"] = {
            "count": args.count,
            "start_ticks": args.start_ticks,
            "tick_step": args.tick_step,
            "tick_hz": args.tick_hz,
            "startup_margin_s": args.startup_margin,
            "channel": args.channel,
            "scale": args.scale,
            "initial_phase": args.initial_phase,
            "phase_increment": args.phase_increment,
            "phase_increment_step": args.phase_increment_step,
            "phase_reinit": args.phase_reinit,
        }

    def exchange(
        sock: socket.socket,
        frame: bytes,
        *,
        seq: int,
        phase: str,
        kind: int,
        count: int,
    ) -> dict:
        try:
            row = _exchange(
                sock,
                frame,
                session_id=session_id,
                seq=seq,
                phase=phase,
                kind=kind,
                record_count=count,
                ack_timeout=args.ack_timeout,
                retries=args.retries,
            )
        except ExchangeError as exc:
            telemetry["frames"].append(exc.row)
            raise
        telemetry["frames"].append(row)
        return row

    started = time.monotonic()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        if args.source_ip or args.source_port:
            sock.bind((args.source_ip or "0.0.0.0", args.source_port))
        sock.connect((args.dest_ip, args.port))

        seq = 0
        open_frame = _build_frame(
            session_id,
            seq,
            KIND_CONTROL,
            FLAG_OPEN,
            program_digest,
        )
        exchange(
            sock,
            open_frame,
            seq=seq,
            phase="open",
            kind=KIND_CONTROL,
            count=1,
        )

        data_started = time.monotonic()
        sent_records = 0
        for offset in range(0, len(records), args.batch * RECORD_BYTES):
            batch_records = records[offset:offset + args.batch * RECORD_BYTES]
            batch_count = len(batch_records) // RECORD_BYTES
            seq = (seq + 1) & 0xFFFFFFFF
            data_frame = _build_frame(
                session_id,
                seq,
                payload_kind,
                0,
                batch_records,
            )
            exchange(
                sock,
                data_frame,
                seq=seq,
                phase="data",
                kind=payload_kind,
                count=batch_count,
            )
            sent_records += batch_count

            if args.rate > 0:
                deadline = data_started + sent_records / args.rate
                delay = deadline - time.monotonic()
                if delay > 0:
                    time.sleep(delay)

        seq = (seq + 1) & 0xFFFFFFFF
        close_flags = FLAG_CLOSE_WITH_EOF if payload_kind == KIND_EVENTS else 0
        close_frame = _build_frame(
            session_id,
            seq,
            KIND_CONTROL,
            close_flags,
            b"",
        )
        close_row = exchange(
            sock,
            close_frame,
            seq=seq,
            phase="close",
            kind=KIND_CONTROL,
            count=0,
        )

    elapsed = time.monotonic() - started
    summary = {
        "accepted": True,
        "execution_verified": False,
        "result_scope": "GWAS/2 transport acceptance only",
        "protocol": "GWAS/2",
        "session_id": f"0x{session_id:08X}",
        "payload_kind": args.kind,
        "records": record_count,
        "data_frames": (record_count + args.batch - 1) // args.batch,
        "elapsed_s": elapsed,
        "effective_records_per_s": record_count / elapsed if elapsed else 0.0,
        "final_ack": close_row["ack"],
    }
    telemetry["summary"] = summary
    return summary


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kind", choices=("direct", "c1"), default="direct")
    parser.add_argument(
        "--records-file",
        type=Path,
        help="raw file containing contiguous 32-byte direct or C1 records",
    )
    parser.add_argument("--dest-ip", default="192.0.2.2")
    parser.add_argument("--source-ip")
    parser.add_argument("--port", type=int, default=5000)
    parser.add_argument("--source-port", type=int, default=0)
    parser.add_argument("--batch", type=int, default=45)
    parser.add_argument(
        "--mtu",
        type=int,
        default=DEFAULT_IPV4_MTU,
        help="host IPv4 MTU; batch is rejected if it would fragment",
    )
    parser.add_argument(
        "--rate",
        type=float,
        default=0.0,
        help="target data-record rate; zero disables pacing",
    )
    parser.add_argument("--ack-timeout", type=float, default=0.25)
    parser.add_argument("--retries", type=int, default=3)
    parser.add_argument(
        "--session-id",
        type=_auto_int,
        help="fixed 32-bit session ID; default is random for each run",
    )
    parser.add_argument("--telemetry", type=Path)

    generation = parser.add_argument_group(
        "deterministic direct-event generation (used without --records-file)"
    )
    generation.add_argument("--count", type=int, default=1024)
    generation.add_argument(
        "--start-ticks",
        type=_auto_int,
        help="first event tick; default allows upload time plus startup margin",
    )
    generation.add_argument(
        "--tick-hz",
        type=float,
        default=245_760_000.0,
        help="scheduler tick frequency used for automatic start-ticks",
    )
    generation.add_argument(
        "--startup-margin",
        type=float,
        default=0.25,
        help="extra seconds before an automatically timed first event",
    )
    generation.add_argument("--tick-step", type=_auto_int, default=32)
    generation.add_argument("--channel", type=_auto_int, default=0)
    generation.add_argument("--scale", type=_auto_int, default=0)
    generation.add_argument("--initial-phase", type=_auto_int, default=0)
    generation.add_argument("--phase-increment", type=_auto_int, default=0)
    generation.add_argument("--phase-increment-step", type=_auto_int, default=0)
    generation.add_argument("--phase-reinit", action="store_true")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    telemetry = {
        "started_utc": datetime.now(timezone.utc).isoformat(),
        "frames": [],
    }
    try:
        summary = _send_program(args, telemetry)
    except (ExchangeError, OSError, ProtocolError, ValueError) as exc:
        telemetry["ok"] = False
        telemetry["error"] = str(exc)
        telemetry["finished_utc"] = datetime.now(timezone.utc).isoformat()
        if args.telemetry:
            _write_telemetry(args.telemetry, telemetry)
        print(f"awg_stream_sender_v2: {exc}", file=sys.stderr)
        return 2

    telemetry["ok"] = True
    telemetry["finished_utc"] = datetime.now(timezone.utc).isoformat()
    if args.telemetry:
        _write_telemetry(args.telemetry, telemetry)
    print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
