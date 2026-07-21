#!/usr/bin/env python3
"""Capture Phase F UART bytes and a UTC-timestamped text view.

The capture has no knowledge of firmware prompts. If --duration is omitted,
capture continues until Ctrl+C. The --output value is a base path; the script
creates "<output>.raw" and "<output>.log". Use --interactive to send console
commands through the same serial connection while logging replies.
"""

from __future__ import annotations

import argparse
import sys
import threading
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import BinaryIO, Optional, Sequence, TextIO

try:
    import serial
except ImportError:  # pragma: no cover - depends on the operator environment
    serial = None


def utc_now() -> str:
    """Return an ISO-8601 UTC timestamp with millisecond resolution."""

    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--serial-port",
        required=True,
        help="UART device, for example COM4 or /dev/ttyUSB0",
    )
    parser.add_argument(
        "--baudrate",
        type=int,
        default=115200,
        help="UART baud rate (default: 115200)",
    )
    parser.add_argument(
        "--duration",
        type=float,
        help="capture duration in seconds; omit to run until Ctrl+C",
    )
    parser.add_argument(
        "--output",
        required=True,
        type=Path,
        help="absolute or relative base path for the .raw and .log files",
    )
    parser.add_argument(
        "--interactive",
        action="store_true",
        help="send lines typed on stdin; type /quit to stop the capture",
    )
    return parser


def write_text_line(stream: TextIO, line: bytes) -> None:
    """Write one decoded UART line with its line-completion timestamp."""

    text = line.decode("utf-8", errors="replace").strip("\r")
    record = f"[{utc_now()}] {text}\n"
    stream.write(record)
    stream.flush()
    print(record, end="")


def consume_complete_lines(pending: bytearray, stream: TextIO) -> None:
    """Emit newline-terminated lines while preserving all bytes in the raw log."""

    while True:
        try:
            newline = pending.index(0x0A)
        except ValueError:
            return

        line = bytes(pending[:newline])
        del pending[: newline + 1]
        write_text_line(stream, line)


def capture(
    port: "serial.Serial",
    raw_stream: BinaryIO,
    text_stream: TextIO,
    duration: Optional[float],
    stop_event: threading.Event,
) -> tuple[int, bool]:
    """Capture until the deadline or Ctrl+C; return byte count and interruption."""

    pending = bytearray()
    captured_bytes = 0
    interrupted = False
    deadline = None if duration is None else time.monotonic() + duration

    try:
        while (
            not stop_event.is_set()
            and (deadline is None or time.monotonic() < deadline)
        ):
            chunk = port.read(port.in_waiting or 1)
            if not chunk:
                continue

            raw_stream.write(chunk)
            raw_stream.flush()
            captured_bytes += len(chunk)
            pending.extend(chunk)
            consume_complete_lines(pending, text_stream)
    except KeyboardInterrupt:
        interrupted = True

    if pending:
        write_text_line(text_stream, bytes(pending))

    return captured_bytes, interrupted


def interactive_console(
    port: "serial.Serial",
    stop_event: threading.Event,
) -> None:
    """Forward complete stdin lines to the firmware console."""

    while not stop_event.is_set():
        try:
            line = sys.stdin.readline()
        except (KeyboardInterrupt, OSError):
            return
        if line == "":
            return
        command = line.rstrip("\r\n")
        if command == "/quit":
            stop_event.set()
            return
        if not command:
            continue
        try:
            port.write((command + "\r").encode("ascii"))
            port.flush()
        except (OSError, UnicodeEncodeError):
            stop_event.set()
            return


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)

    if serial is None:
        print(
            "capture_phase_f_uart: pyserial is required; install it with "
            "'python -m pip install pyserial'",
            file=sys.stderr,
        )
        return 2
    if args.baudrate <= 0:
        raise ValueError("baudrate must be positive")
    if args.duration is not None and args.duration <= 0:
        raise ValueError("duration must be positive")

    base_path = args.output.expanduser().resolve()
    raw_path = Path(f"{base_path}.raw")
    text_path = Path(f"{base_path}.log")
    base_path.parent.mkdir(parents=True, exist_ok=True)

    for path in (raw_path, text_path):
        if path.exists():
            raise FileExistsError(f"refusing to overwrite existing capture: {path}")

    started = utc_now()
    print(f"Raw UART log: {raw_path}")
    print(f"Timestamped log: {text_path}")
    if args.duration is None:
        print("Capturing until Ctrl+C...")
    else:
        print(f"Capturing for {args.duration:g} seconds...")

    with raw_path.open("xb") as raw_stream, text_path.open(
        "x", encoding="utf-8", newline="\n"
    ) as text_stream:
        text_stream.write(f"# started_utc={started}\n")
        text_stream.write(f"# serial_port={args.serial_port}\n")
        text_stream.write(f"# baudrate={args.baudrate}\n")
        text_stream.write(f"# raw_log={raw_path}\n")
        text_stream.flush()

        with serial.Serial(
            port=args.serial_port,
            baudrate=args.baudrate,
            timeout=0.1,
        ) as uart:
            stop_event = threading.Event()
            if args.interactive:
                print("Interactive console enabled. Type /quit to stop.")
                thread = threading.Thread(
                    target=interactive_console,
                    args=(uart, stop_event),
                    daemon=True,
                )
                thread.start()
            captured_bytes, interrupted = capture(
                uart,
                raw_stream,
                text_stream,
                args.duration,
                stop_event,
            )
            stop_event.set()

        finished = utc_now()
        text_stream.write(f"# finished_utc={finished}\n")
        text_stream.write(f"# captured_bytes={captured_bytes}\n")
        text_stream.write(f"# interrupted={str(interrupted).lower()}\n")

    print(f"Captured {captured_bytes} bytes.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as exc:
        print(f"capture_phase_f_uart: {exc}", file=sys.stderr)
        raise SystemExit(2)
