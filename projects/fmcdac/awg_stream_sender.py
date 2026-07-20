#!/usr/bin/env python3
"""Legacy GWAS/1 diagnostic sender for the FMCDAC Phase-F AWG stream.

Production benchmark runs use the rfsoc-bench GWAS/2 controller, which adds
session identity and direct/C1 payload discrimination.
"""

from __future__ import annotations

import argparse
import csv
import json
import socket
import sys
import time
from dataclasses import asdict
from pathlib import Path
from typing import Iterable, Sequence

from awg_sched_host import (
    AWG_EVENT_V1_SIZE,
    AWG_EVENT_V1_STRUCT,
    AWG_SCHED_FLAG_EOF,
    AWG_STREAM_PROTO_ACK_STRUCT,
    AwgSchedEvent,
    pack_stream_frame,
    unpack_stream_ack,
)


def _load_events(path: Path) -> list[AwgSchedEvent]:
    data = path.read_bytes()
    if not data or len(data) % AWG_EVENT_V1_SIZE:
        raise ValueError("event file must contain a non-empty multiple of 32 bytes")
    events: list[AwgSchedEvent] = []
    for offset in range(0, len(data), AWG_EVENT_V1_SIZE):
        values = AWG_EVENT_V1_STRUCT.unpack_from(data, offset)
        events.append(AwgSchedEvent(*values))
    return events


def _deterministic_events(count: int, start_ticks: int, tick_step: int,
                          channel: int) -> list[AwgSchedEvent]:
    if count <= 0:
        raise ValueError("event count must be positive")
    if tick_step <= 0:
        raise ValueError("tick step must be positive")
    return [
        AwgSchedEvent(
            timestamp_ticks=start_ticks + index * tick_step,
            channel=channel,
            flags=0,
            payload_word0=index & 0xFFFFFFFF,
            payload_word1=0,
            payload_word2=0,
            payload_word3=0,
        )
        for index in range(count)
    ]


def _chunks(events: Sequence[AwgSchedEvent], size: int) -> Iterable[Sequence[AwgSchedEvent]]:
    for offset in range(0, len(events), size):
        yield events[offset:offset + size]


def _send_with_optional_ack(sock: socket.socket, frame: bytes, seq: int,
                            wait_ack: bool, retries: int) -> tuple[int, dict | None]:
    attempts = 0
    while True:
        attempts += 1
        sock.send(frame)
        if not wait_ack:
            return attempts, None
        try:
            data = sock.recv(AWG_STREAM_PROTO_ACK_STRUCT.size)
        except socket.timeout:
            if attempts > retries:
                raise TimeoutError(f"no ACK for sequence {seq} after {attempts} attempts")
            continue
        ack = unpack_stream_ack(data)
        if ack.seq_acked != seq:
            if attempts > retries:
                raise RuntimeError(
                    f"expected ACK sequence {seq}, received {ack.seq_acked}"
                )
            continue
        if not ack.ok:
            raise RuntimeError(f"FPGA rejected sequence {seq}: {ack.status_name}")
        return attempts, asdict(ack)


def _write_telemetry(path: Path, rows: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.suffix.lower() == ".csv":
        with path.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
    else:
        path.write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dest-ip", default="192.0.2.2")
    parser.add_argument("--source-ip")
    parser.add_argument("--port", type=int, default=5000)
    parser.add_argument("--events-file", type=Path)
    parser.add_argument("--count", type=int, default=1024)
    parser.add_argument("--batch", type=int, default=64)
    parser.add_argument("--rate", type=float, default=100_000.0,
                        help="target event rate; zero disables pacing")
    parser.add_argument("--duration", type=float,
                        help="stress duration; derives count from rate")
    parser.add_argument("--start-ticks", type=int, default=1_000_000)
    parser.add_argument("--tick-step", type=int, default=32)
    parser.add_argument("--channel", type=int, default=0)
    parser.add_argument("--wait-ack", action="store_true")
    parser.add_argument("--ack-timeout", type=float, default=0.25)
    parser.add_argument("--retries", type=int, default=3)
    parser.add_argument("--telemetry", type=Path)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if not 1 <= args.batch <= 128:
        raise ValueError("batch must be in the range 1..128")
    if not 0 <= args.port <= 65535:
        raise ValueError("port must fit in 16 bits")
    if args.rate < 0:
        raise ValueError("rate must not be negative")
    if args.duration is not None:
        if args.duration <= 0 or args.rate <= 0:
            raise ValueError("duration and rate must both be positive")
        args.count = max(1, int(args.duration * args.rate))

    events = (
        _load_events(args.events_file)
        if args.events_file
        else _deterministic_events(args.count, args.start_ticks,
                                   args.tick_step, args.channel)
    )
    # The protocol owns EOF placement on CLOSE_WITH_EOF.
    events = [
        AwgSchedEvent(
            event.timestamp_ticks, event.channel,
            event.flags & ~AWG_SCHED_FLAG_EOF,
            event.payload_word0, event.payload_word1,
            event.payload_word2, event.payload_word3, event.reserved,
        )
        for event in events
    ]
    batches = list(_chunks(events, args.batch))
    telemetry: list[dict] = []

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        if args.source_ip:
            sock.bind((args.source_ip, 0))
        sock.connect((args.dest_ip, args.port))
        sock.settimeout(args.ack_timeout)
        started = time.monotonic()
        sent_events = 0

        for index, batch in enumerate(batches):
            seq = index & 0xFFFFFFFF
            frame = pack_stream_frame(
                batch,
                seq=seq,
                open_stream=(index == 0),
                close_with_eof=(index == len(batches) - 1),
            )
            frame_started = time.monotonic()
            attempts, ack = _send_with_optional_ack(
                sock, frame, seq, args.wait_ack, args.retries
            )
            sent_events += len(batch)
            row = {
                "seq": seq,
                "events": len(batch),
                "frame_bytes": len(frame),
                "attempts": attempts,
                "elapsed_s": time.monotonic() - started,
            }
            if ack:
                row.update({f"ack_{key}": value for key, value in ack.items()})
            telemetry.append(row)

            if args.rate > 0:
                deadline = started + sent_events / args.rate
                delay = deadline - time.monotonic()
                if delay > 0:
                    time.sleep(delay)
            row["frame_service_s"] = time.monotonic() - frame_started

    elapsed = time.monotonic() - started
    summary = {
        "destination": f"{args.dest_ip}:{args.port}",
        "events": len(events),
        "frames": len(batches),
        "elapsed_s": elapsed,
        "effective_events_per_s": len(events) / elapsed if elapsed else 0.0,
        "wait_ack": args.wait_ack,
    }
    print(json.dumps(summary, sort_keys=True))
    if args.telemetry:
        _write_telemetry(args.telemetry, telemetry)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, TimeoutError, ValueError) as exc:
        print(f"awg_stream_sender: {exc}", file=sys.stderr)
        raise SystemExit(2)
