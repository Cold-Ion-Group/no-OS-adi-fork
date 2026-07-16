#!/usr/bin/env python3
"""
Host-side helpers for the FMCDAC AWG scheduler UART control path.
"""

from __future__ import annotations

import binascii
import math
import re
import struct
from dataclasses import dataclass
from typing import Iterable, List, Optional


AWG_SCHED_FLAG_PHASE_REINIT = 0x0001
AWG_SCHED_FLAG_EOF = 0x0002
AWG_EVENT_V1_SIZE = 32
AWG_EVENT_V1_STRUCT = struct.Struct("<QHHIIIII")
AWG_SCHED_DEFAULT_STARTUP_MARGIN_US = 20_000
AWG_STREAM_PROTO_MAGIC = 0x53415747
AWG_STREAM_PROTO_HEADER_STRUCT = struct.Struct("<IIHH")
AWG_STREAM_PROTO_ACK_STRUCT = struct.Struct("<IIIIIII")
AWG_STREAM_PROTO_MAX_FRAME_EVENTS = 128
AWG_STREAM_PROTO_FLAG_OPEN = 0x0001
AWG_STREAM_PROTO_FLAG_CLOSE_WITH_EOF = 0x0002
AWG_STREAM_ACK_STATUS_NAMES = {
    1 << 0: "bad_arg",
    1 << 1: "bad_magic",
    1 << 2: "bad_length",
    1 << 3: "bad_crc",
    1 << 4: "disabled",
    1 << 5: "open_failed",
    1 << 6: "ring_full",
    1 << 7: "overflow",
    1 << 8: "scheduler_error",
    1 << 9: "close_failed",
    1 << 10: "bad_sequence",
    1 << 11: "bad_flags",
    1 << 12: "bad_session",
    1 << 13: "dma_error",
    1 << 14: "bad_event",
}
AWG_STREAM_UART_RAW_BAUD = 115200
AWG_STREAM_UART_RAW_BYTES_PER_S = AWG_STREAM_UART_RAW_BAUD / 10.0
AWG_STREAM_UART_HEX_BYTES_PER_EVENT = AWG_EVENT_V1_SIZE * 2
AWG_STREAM_UART_EXPECTED_EVENTS_PER_S = (100.0, 150.0)
AWG_CONSOLE_READY_MARKER = "[AWG-UART] Ready."
AWG_CONSOLE_LOAD_READY_MARKER = "[AWG-UART] LOADBIN READY"
AWG_CONSOLE_LOAD_RX_BEGIN_MARKER = "[AWG-UART] LOADBIN RX BEGIN"
AWG_CONSOLE_LOAD_OK_MARKER = "[AWG-UART] LOADBIN OK"
AWG_CONSOLE_RUN_DONE_MARKER = "[AWG-UART] RUN DONE"
AWG_CONSOLE_ABORT_DONE_MARKER = "[AWG-UART] ABORT DONE"
AWG_CONSOLE_ERROR_MARKER = "[AWG-UART] ERROR "
AWG_CONSOLE_ARTIFACT_BEGIN = "[AWG-UART] ARTIFACT_BEGIN"
AWG_CONSOLE_ARTIFACT_END = "[AWG-UART] ARTIFACT_END"
AWG_STREAM_READY_MARKER = "[AWG-STREAM] STREAMHEX READY"
AWG_STREAM_RX_BEGIN_MARKER = "[AWG-STREAM] STREAMHEX RX BEGIN"
AWG_STREAM_ACK_PREFIX = "[AWG-STREAM] ACK "
AWG_STREAM_STATUS_PREFIX = "[AWG-STREAM] STATUS "
AWG_STREAM_ERROR_MARKER = "[AWG-STREAM] ERROR "

_INFO_RE = re.compile(
    r"^\[AWG-UART\] INFO "
    r"base=0x(?P<base>[0-9A-Fa-f]+) "
    r"max_events=(?P<max_events>\d+) "
    r"tick_hz=(?P<tick_hz>\d+) "
    r"timeout_ms=(?P<timeout_ms>\d+) "
    r"dds_clock_hz=(?P<dds_clock_hz>\d+) "
    r"dds_phase_dw=(?P<dds_phase_dw>\d+) "
    r"loaded=(?P<loaded>\d+) "
    r"configured=(?P<configured>[01])$"
)
_ARTIFACT_CONFIG_RE = re.compile(
    r"^\[SCHED-ARTIFACT\] config "
    r"base=0x(?P<base>[0-9A-Fa-f]+) "
    r"max_events=(?P<max_events>\d+) "
    r"tick_hz=(?P<tick_hz>\d+) "
    r"timeout_ms=(?P<timeout_ms>\d+)$"
)
_ARTIFACT_EVENT_RE = re.compile(
    r"^\[SCHED-ARTIFACT\] event "
    r"idx=(?P<idx>\d+) "
    r"ts=0x(?P<ts_hi>[0-9A-Fa-f]+)_(?P<ts_lo>[0-9A-Fa-f]+) "
    r"ch=(?P<channel>\d+) "
    r"fl=0x(?P<flags>[0-9A-Fa-f]+) "
    r"p0=0x(?P<p0>[0-9A-Fa-f]+) "
    r"p1=0x(?P<p1>[0-9A-Fa-f]+) "
    r"p2=0x(?P<p2>[0-9A-Fa-f]+) "
    r"p3=0x(?P<p3>[0-9A-Fa-f]+)$"
)
_ARTIFACT_STATUS_RE = re.compile(
    r"^\[SCHED-ARTIFACT\] status "
    r"armed=(?P<armed>[01]) "
    r"running=(?P<running>[01]) "
    r"done=(?P<done>[01]) "
    r"error=(?P<error>[01]) "
    r"err_code=0x(?P<err_code>[0-9A-Fa-f]+) "
    r"current=(?P<current>\d+) "
    r"loaded=(?P<loaded>\d+) "
    r"commit=(?P<commit>\d+) "
    r"reinit=(?P<reinit>\d+) "
    r"reinit_reject=(?P<reinit_reject>\d+) "
    r"irq=0x(?P<irq>[0-9A-Fa-f]+)$"
)
_ARTIFACT_TIME_RE = re.compile(
    r"^\[SCHED-ARTIFACT\] time_now=0x(?P<time_hi>[0-9A-Fa-f]+)_(?P<time_lo>[0-9A-Fa-f]+) "
    r"last_exec=0x(?P<last_hi>[0-9A-Fa-f]+)_(?P<last_lo>[0-9A-Fa-f]+)$"
)
_ARTIFACT_STREAM_RE = re.compile(
    r"^\[SCHED-ARTIFACT\] stream "
    r"depth=(?P<depth>\d+) "
    r"low_wmark=(?P<low_wmark>\d+) "
    r"ctrl=0x(?P<ctrl>[0-9A-Fa-f]+) "
    r"occupancy=(?P<occupancy>\d+) "
    r"free_space=(?P<free_space>\d+) "
    r"pushes=(?P<pushes>\d+) "
    r"stalls=(?P<stalls>\d+) "
    r"irq=0x(?P<irq>[0-9A-Fa-f]+) "
    r"err=0x(?P<err>[0-9A-Fa-f]+)$"
)
_STREAM_ACK_RE = re.compile(
    r"^\[AWG-STREAM\] ACK "
    r"magic=0x(?P<magic>[0-9A-Fa-f]+) "
    r"seq=(?P<seq>\d+) "
    r"ddr_free=(?P<ddr_free>\d+) "
    r"status=(?P<status>\d+) "
    r"stream_free=(?P<stream_free>\d+) "
    r"stalls=(?P<stalls>\d+) "
    r"irq=0x(?P<irq>[0-9A-Fa-f]+) "
    r"ret=(?P<ret>-?\d+) "
    r"bytes=(?P<bytes>\d+) "
    r"events=(?P<events>\d+) "
    r"flags=0x(?P<flags>[0-9A-Fa-f]+)$"
)
_STREAM_STATUS_RE = re.compile(
    r"^\[AWG-STREAM\] STATUS "
    r"tag=(?P<tag>\S+) "
    r"ip_id=0x(?P<ip_id>[0-9A-Fa-f]+) "
    r"ip_version=0x(?P<ip_version>[0-9A-Fa-f]+) "
    r"stream_depth=(?P<stream_depth>\d+) "
    r"low_wmark=(?P<low_wmark>\d+) "
    r"stream_ctrl=0x(?P<stream_ctrl>[0-9A-Fa-f]+) "
    r"occupancy=(?P<occupancy>\d+) "
    r"free_space=(?P<free_space>\d+) "
    r"stream_pushes=(?P<stream_pushes>\d+) "
    r"stream_stalls=(?P<stream_stalls>\d+) "
    r"commit=(?P<commit>\d+) "
    r"err=0x(?P<err>[0-9A-Fa-f]+) "
    r"irq=0x(?P<irq>[0-9A-Fa-f]+) "
    r"hw_status=0x(?P<hw_status>[0-9A-Fa-f]+) "
    r"mode=(?P<mode>[01]) "
    r"overflow=(?P<overflow>[01]) "
    r"eof_seen=(?P<eof_seen>[01]) "
    r"running=(?P<running>[01]) "
    r"done=(?P<done>[01]) "
    r"error=(?P<error>[01])$"
)


@dataclass(frozen=True)
class AwgSchedInfo:
    base_addr: int
    max_events: int
    tick_hz: int
    timeout_ms: int
    dds_clock_hz: int
    dds_phase_dw: int
    loaded_count: int
    configured: bool


@dataclass(frozen=True)
class AwgSchedEvent:
    timestamp_ticks: int
    channel: int
    flags: int
    payload_word0: int
    payload_word1: int
    payload_word2: int
    payload_word3: int
    reserved: int = 0

    def pack(self) -> bytes:
        return AWG_EVENT_V1_STRUCT.pack(
            self.timestamp_ticks,
            self.channel,
            self.flags,
            self.payload_word0,
            self.payload_word1,
            self.payload_word2,
            self.payload_word3,
            self.reserved,
        )


@dataclass(frozen=True)
class AwgSchedArtifactConfig:
    base_addr: int
    max_events: int
    tick_hz: int
    timeout_ms: int


@dataclass(frozen=True)
class AwgSchedArtifactStatus:
    armed: bool
    running: bool
    done: bool
    error: bool
    err_code: int
    current_event: int
    loaded_events: int
    commit_count: int
    reinit_count: int
    reinit_reject_count: int
    irq_status: int


@dataclass(frozen=True)
class AwgSchedArtifactTime:
    time_now: int
    last_exec: int


@dataclass(frozen=True)
class AwgSchedArtifactStream:
    stream_depth: int
    low_wmark: int
    stream_ctrl: int
    occupancy: int
    free_space: int
    stream_pushes: int
    stream_stalls: int
    irq_status: int
    err_reg: int


@dataclass(frozen=True)
class AwgSchedArtifactBlock:
    config: Optional[AwgSchedArtifactConfig]
    events: List[AwgSchedEvent]
    status: Optional[AwgSchedArtifactStatus]
    time: Optional[AwgSchedArtifactTime]
    stream: Optional[AwgSchedArtifactStream]


@dataclass(frozen=True)
class AwgStreamAck:
    magic: int
    seq_acked: int
    ddr_free_events: int
    status: int
    stream_free_events: int
    stream_stalls: int
    irq_status: int
    ret: int
    frame_bytes: int
    event_count: int
    flags: int

    @property
    def status_name(self) -> str:
        return stream_ack_status_name(self.status)


@dataclass(frozen=True)
class AwgStreamWireAck:
    magic: int
    seq_acked: int
    ddr_free_events: int
    status: int
    stream_free_events: int
    stream_stalls: int
    irq_status: int

    @property
    def ok(self) -> bool:
        return self.magic == AWG_STREAM_PROTO_MAGIC and self.status == 0

    @property
    def status_name(self) -> str:
        return stream_ack_status_name(self.status)


@dataclass(frozen=True)
class AwgStreamStatus:
    tag: str
    ip_id: int
    ip_version: int
    stream_depth: int
    low_wmark: int
    stream_ctrl: int
    occupancy: int
    free_space: int
    stream_pushes: int
    stream_stalls: int
    commit_count: int
    err_reg: int
    irq_status: int
    hw_status: int
    mode: bool
    overflow: bool
    eof_seen: bool
    running: bool
    done: bool
    error: bool


def parse_info_line(line: str) -> AwgSchedInfo:
    match = _INFO_RE.match(line.strip())
    if not match:
        raise ValueError(f"Unrecognized AWG scheduler info line: {line!r}")

    return AwgSchedInfo(
        base_addr=int(match.group("base"), 16),
        max_events=int(match.group("max_events")),
        tick_hz=int(match.group("tick_hz")),
        timeout_ms=int(match.group("timeout_ms")),
        dds_clock_hz=int(match.group("dds_clock_hz")),
        dds_phase_dw=int(match.group("dds_phase_dw")),
        loaded_count=int(match.group("loaded")),
        configured=(match.group("configured") == "1"),
    )


def _u64_from_parts(hi_hex: str, lo_hex: str) -> int:
    return (int(hi_hex, 16) << 32) | int(lo_hex, 16)


def parse_artifact_blocks(text: str) -> List[AwgSchedArtifactBlock]:
    blocks: List[AwgSchedArtifactBlock] = []
    active_lines: List[str] = []
    in_block = False

    for raw_line in text.splitlines():
        line = raw_line.strip()
        if line == AWG_CONSOLE_ARTIFACT_BEGIN:
            active_lines = []
            in_block = True
            continue
        if line == AWG_CONSOLE_ARTIFACT_END:
            if in_block:
                blocks.append(_parse_artifact_block_lines(active_lines))
            active_lines = []
            in_block = False
            continue
        if in_block:
            active_lines.append(line)

    if not blocks:
        artifact_lines = [
            line.strip()
            for line in text.splitlines()
            if line.strip().startswith("[SCHED-ARTIFACT]")
        ]
        if artifact_lines:
            blocks.append(_parse_artifact_block_lines(artifact_lines))

    return blocks


def parse_last_artifact_block(text: str) -> AwgSchedArtifactBlock:
    blocks = parse_artifact_blocks(text)
    if not blocks:
        raise ValueError("No scheduler artifact block found")
    return blocks[-1]


def _parse_artifact_block_lines(lines: Iterable[str]) -> AwgSchedArtifactBlock:
    config: Optional[AwgSchedArtifactConfig] = None
    events: List[AwgSchedEvent] = []
    status: Optional[AwgSchedArtifactStatus] = None
    time_info: Optional[AwgSchedArtifactTime] = None
    stream_info: Optional[AwgSchedArtifactStream] = None

    for line in lines:
        match = _ARTIFACT_CONFIG_RE.match(line)
        if match:
            config = AwgSchedArtifactConfig(
                base_addr=int(match.group("base"), 16),
                max_events=int(match.group("max_events")),
                tick_hz=int(match.group("tick_hz")),
                timeout_ms=int(match.group("timeout_ms")),
            )
            continue

        match = _ARTIFACT_EVENT_RE.match(line)
        if match:
            events.append(
                AwgSchedEvent(
                    timestamp_ticks=_u64_from_parts(
                        match.group("ts_hi"), match.group("ts_lo")
                    ),
                    channel=int(match.group("channel")),
                    flags=int(match.group("flags"), 16),
                    payload_word0=int(match.group("p0"), 16),
                    payload_word1=int(match.group("p1"), 16),
                    payload_word2=int(match.group("p2"), 16),
                    payload_word3=int(match.group("p3"), 16),
                )
            )
            continue

        match = _ARTIFACT_STATUS_RE.match(line)
        if match:
            status = AwgSchedArtifactStatus(
                armed=(match.group("armed") == "1"),
                running=(match.group("running") == "1"),
                done=(match.group("done") == "1"),
                error=(match.group("error") == "1"),
                err_code=int(match.group("err_code"), 16),
                current_event=int(match.group("current")),
                loaded_events=int(match.group("loaded")),
                commit_count=int(match.group("commit")),
                reinit_count=int(match.group("reinit")),
                reinit_reject_count=int(match.group("reinit_reject")),
                irq_status=int(match.group("irq"), 16),
            )
            continue

        match = _ARTIFACT_TIME_RE.match(line)
        if match:
            time_info = AwgSchedArtifactTime(
                time_now=_u64_from_parts(match.group("time_hi"), match.group("time_lo")),
                last_exec=_u64_from_parts(match.group("last_hi"), match.group("last_lo")),
            )
            continue

        match = _ARTIFACT_STREAM_RE.match(line)
        if match:
            stream_info = AwgSchedArtifactStream(
                stream_depth=int(match.group("depth")),
                low_wmark=int(match.group("low_wmark")),
                stream_ctrl=int(match.group("ctrl"), 16),
                occupancy=int(match.group("occupancy")),
                free_space=int(match.group("free_space")),
                stream_pushes=int(match.group("pushes")),
                stream_stalls=int(match.group("stalls")),
                irq_status=int(match.group("irq"), 16),
                err_reg=int(match.group("err"), 16),
            )

    return AwgSchedArtifactBlock(
        config=config,
        events=events,
        status=status,
        time=time_info,
        stream=stream_info,
    )


def pack_events(events: Iterable[AwgSchedEvent]) -> bytes:
    return b"".join(event.pack() for event in events)


def stream_crc32_ieee(data: bytes) -> int:
    return binascii.crc32(data) & 0xFFFFFFFF


def pack_stream_frame(
    events: Iterable[AwgSchedEvent],
    *,
    seq: int,
    open_stream: bool = False,
    close_with_eof: bool = False,
    corrupt_crc: bool = False,
) -> bytes:
    event_list = list(events)
    if len(event_list) > AWG_STREAM_PROTO_MAX_FRAME_EVENTS:
        raise ValueError(
            f"stream frame cannot carry more than {AWG_STREAM_PROTO_MAX_FRAME_EVENTS} events"
        )
    if open_stream and (seq & 0xFFFFFFFF) != 0:
        raise ValueError("an OPEN frame must use sequence zero")
    if close_with_eof and not event_list:
        raise ValueError("CLOSE_WITH_EOF requires at least one event")

    flags = 0
    if open_stream:
        flags |= AWG_STREAM_PROTO_FLAG_OPEN
    if close_with_eof:
        flags |= AWG_STREAM_PROTO_FLAG_CLOSE_WITH_EOF

    body = AWG_STREAM_PROTO_HEADER_STRUCT.pack(
        AWG_STREAM_PROTO_MAGIC,
        seq & 0xFFFFFFFF,
        len(event_list),
        flags,
    ) + pack_events(event_list)
    crc = stream_crc32_ieee(body)
    if corrupt_crc:
        crc ^= 0xFFFFFFFF
    return body + struct.pack("<I", crc)


def stream_ack_status_name(status: int) -> str:
    if status == 0:
        return "ok"
    names = [
        name for bit, name in AWG_STREAM_ACK_STATUS_NAMES.items() if status & bit
    ]
    known_mask = sum(AWG_STREAM_ACK_STATUS_NAMES)
    unknown = status & ~known_mask
    if unknown:
        names.append(f"unknown_0x{unknown:08x}")
    return "|".join(names)


def unpack_stream_ack(data: bytes) -> AwgStreamWireAck:
    if len(data) != AWG_STREAM_PROTO_ACK_STRUCT.size:
        raise ValueError(
            f"AWG stream ACK must be {AWG_STREAM_PROTO_ACK_STRUCT.size} bytes"
        )
    return AwgStreamWireAck(*AWG_STREAM_PROTO_ACK_STRUCT.unpack(data))


def stream_frame_wire_stats(frame: bytes, *, elapsed_s: Optional[float] = None) -> dict:
    hex_chars = len(frame) * 2
    stats = {
        "frame_bytes": len(frame),
        "frame_hex_chars": hex_chars,
        "raw_uart_baud": AWG_STREAM_UART_RAW_BAUD,
        "raw_uart_bytes_per_s": AWG_STREAM_UART_RAW_BYTES_PER_S,
        "ascii_hex_event_wire_bytes": AWG_STREAM_UART_HEX_BYTES_PER_EVENT,
        "expected_sustained_events_per_s": list(AWG_STREAM_UART_EXPECTED_EVENTS_PER_S),
        "is_transport_correctness_path": True,
    }
    if elapsed_s is not None and elapsed_s > 0:
        stats["wall_clock_s"] = elapsed_s
        stats["wire_hex_chars_per_s"] = hex_chars / elapsed_s
    return stats


def estimate_uartlite_stream_seconds(event_count: int, events_per_s: float = 125.0) -> float:
    if event_count < 0:
        raise ValueError("event_count must be non-negative")
    if events_per_s <= 0:
        raise ValueError("events_per_s must be positive")
    return event_count / events_per_s


def parse_stream_ack_line(line: str) -> AwgStreamAck:
    match = _STREAM_ACK_RE.match(line.strip())
    if not match:
        raise ValueError(f"Unrecognized AWG stream ACK line: {line!r}")

    return AwgStreamAck(
        magic=int(match.group("magic"), 16),
        seq_acked=int(match.group("seq")),
        ddr_free_events=int(match.group("ddr_free")),
        status=int(match.group("status")),
        stream_free_events=int(match.group("stream_free")),
        stream_stalls=int(match.group("stalls")),
        irq_status=int(match.group("irq"), 16),
        ret=int(match.group("ret")),
        frame_bytes=int(match.group("bytes")),
        event_count=int(match.group("events")),
        flags=int(match.group("flags"), 16),
    )


def parse_stream_status_line(line: str) -> AwgStreamStatus:
    match = _STREAM_STATUS_RE.match(line.strip())
    if not match:
        raise ValueError(f"Unrecognized AWG stream status line: {line!r}")

    return AwgStreamStatus(
        tag=match.group("tag"),
        ip_id=int(match.group("ip_id"), 16),
        ip_version=int(match.group("ip_version"), 16),
        stream_depth=int(match.group("stream_depth")),
        low_wmark=int(match.group("low_wmark")),
        stream_ctrl=int(match.group("stream_ctrl"), 16),
        occupancy=int(match.group("occupancy")),
        free_space=int(match.group("free_space")),
        stream_pushes=int(match.group("stream_pushes")),
        stream_stalls=int(match.group("stream_stalls")),
        commit_count=int(match.group("commit")),
        err_reg=int(match.group("err"), 16),
        irq_status=int(match.group("irq"), 16),
        hw_status=int(match.group("hw_status"), 16),
        mode=(match.group("mode") == "1"),
        overflow=(match.group("overflow") == "1"),
        eof_seen=(match.group("eof_seen") == "1"),
        running=(match.group("running") == "1"),
        done=(match.group("done") == "1"),
        error=(match.group("error") == "1"),
    )


def awg_scale_reg_from_u(scale_u: int) -> int:
    scale_reg = abs(scale_u)
    if scale_reg >= 1_999_000:
        scale_reg = 1_999_000
    scale_reg = (scale_reg * 0x4000) // 1_000_000
    if scale_u < 0:
        scale_reg |= 0x8000
    return scale_reg & 0xFFFF


def awg_phase_init_from_mdeg(dds_phase_dw: int, phase_mdeg: int) -> int:
    if dds_phase_dw <= 0 or dds_phase_dw > 32:
        raise ValueError("dds_phase_dw must be in the range 1..32")
    phase_modulus = 1 << dds_phase_dw
    return int(((phase_mdeg * phase_modulus) + (360000 // 2)) // 360000) & (phase_modulus - 1)


def awg_ftw_from_hz(dds_clock_hz: int, freq_hz: int, dds_phase_dw: int) -> int:
    if dds_clock_hz <= 0:
        raise ValueError("dds_clock_hz must be greater than 0")
    if dds_phase_dw <= 0 or dds_phase_dw > 32:
        raise ValueError("dds_phase_dw must be in the range 1..32")
    ftw_modulus = 1 << dds_phase_dw
    return int((freq_hz * ftw_modulus) // dds_clock_hz) & (ftw_modulus - 1)


def pack_awg_payload_v1(*, scale: int, init: int, incr: int, dds_phase_dw: int) -> tuple[int, int, int, int]:
    if dds_phase_dw <= 0 or dds_phase_dw > 32:
        raise ValueError("dds_phase_dw must be in the range 1..32")

    phase_mask = (1 << dds_phase_dw) - 1
    payload = (scale & 0xFFFF)
    payload |= (init & phase_mask) << 16
    payload |= (incr & phase_mask) << (16 + dds_phase_dw)
    return (
        payload & 0xFFFFFFFF,
        (payload >> 32) & 0xFFFFFFFF,
        (payload >> 64) & 0xFFFFFFFF,
        (payload >> 96) & 0xFFFFFFFF,
    )


def build_uniform_freq_list(start_hz: float, stop_hz: float, step_hz: float) -> List[int]:
    if start_hz <= 0 or stop_hz <= 0 or step_hz <= 0:
        raise ValueError("start/stop/step must all be greater than 0")
    if stop_hz < start_hz:
        raise ValueError("stop_hz must be greater than or equal to start_hz")
    steps_float = (stop_hz - start_hz) / step_hz
    steps_rounded = round(steps_float)
    if not math.isclose(steps_float, steps_rounded, rel_tol=0.0, abs_tol=1e-9):
        raise ValueError("sweep must land exactly on the stop frequency")

    return [
        int(round(start_hz + (index * step_hz)))
        for index in range(int(steps_rounded) + 1)
    ]


def build_awg_sweep_events(
    freqs_hz: Iterable[int],
    *,
    tick_hz: int,
    dds_clock_hz: int,
    dds_phase_dw: int,
    tone: int,
    scale_u: int,
    start_ticks: int,
    dwell_us: int,
    channel: int = 0,
    phase_mdeg: int = 0,
) -> List[AwgSchedEvent]:
    if tick_hz <= 0:
        raise ValueError("tick_hz must be greater than 0")
    if dds_clock_hz <= 0:
        raise ValueError("dds_clock_hz must be greater than 0")
    if dwell_us <= 0:
        raise ValueError("dwell_us must be greater than 0")
    if tone != 0:
        raise ValueError("scheduler payload does not carry tone select; tone must be 0")
    if channel < 0 or channel > 0xFFFF:
        raise ValueError("channel must fit in 16 bits")

    freq_list = list(freqs_hz)
    if not freq_list:
        raise ValueError("at least one frequency is required")

    step_ticks = max(1, (tick_hz * dwell_us) // 1_000_000)
    startup_margin_ticks = max(1, (tick_hz * AWG_SCHED_DEFAULT_STARTUP_MARGIN_US) // 1_000_000)
    start_tick_value = max(start_ticks, step_ticks, startup_margin_ticks)
    scale_reg = awg_scale_reg_from_u(scale_u)
    phase_init = awg_phase_init_from_mdeg(dds_phase_dw, phase_mdeg)

    events: List[AwgSchedEvent] = []
    for index, freq_hz in enumerate(freq_list):
        if freq_hz < 0:
            raise ValueError("frequency must be non-negative")
        payload_word0, payload_word1, payload_word2, payload_word3 = pack_awg_payload_v1(
            scale=scale_reg,
            init=phase_init,
            incr=awg_ftw_from_hz(dds_clock_hz, freq_hz, dds_phase_dw),
            dds_phase_dw=dds_phase_dw,
        )
        events.append(
            AwgSchedEvent(
                timestamp_ticks=start_tick_value + (index * step_ticks),
                channel=channel,
                flags=AWG_SCHED_FLAG_PHASE_REINIT if index == 0 else 0,
                payload_word0=payload_word0,
                payload_word1=payload_word1,
                payload_word2=payload_word2,
                payload_word3=payload_word3,
            )
        )

    return events
