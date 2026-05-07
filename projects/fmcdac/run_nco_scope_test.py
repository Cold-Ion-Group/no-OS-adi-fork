#!/usr/bin/env python3
"""
Coordinate the FMCDAC UART diagnostics with R&S FSH8 spectrum-analyzer captures.

The primary workflow is now DDS-centric:
1. Open the DUT UART.
2. Optionally launch the existing FMCDAC ELF through XSDB.
3. Wait for the paused DDS-band, SFDR, throughput, and UART RTT prompts.
4. Advance each paused diagnostic step.
5. For every DDS/SFDR pause, configure the FSH8, capture the needed data, and
   save per-step artifacts plus a run summary.

The older NCO discriminator path is still available, but it is now opt-in.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import re
import struct
import subprocess
import sys
import time
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable, List, Optional, Sequence, Tuple

from dds_band_plot import load_dds_band_series, write_dds_band_csv, write_dds_band_svg
from awg_sched_host import (
    AWG_CONSOLE_ERROR_MARKER,
    AWG_CONSOLE_LOAD_OK_MARKER,
    AWG_CONSOLE_LOAD_READY_MARKER,
    AWG_CONSOLE_LOAD_RX_BEGIN_MARKER,
    AWG_CONSOLE_READY_MARKER,
    AWG_CONSOLE_RUN_DONE_MARKER,
    AWG_EVENT_V1_SIZE,
    AWG_STREAM_ACK_PREFIX,
    AWG_STREAM_ERROR_MARKER,
    AWG_STREAM_PROTO_MAGIC,
    AWG_STREAM_READY_MARKER,
    AWG_STREAM_RX_BEGIN_MARKER,
    AWG_STREAM_UART_EXPECTED_EVENTS_PER_S,
    AWG_STREAM_UART_HEX_BYTES_PER_EVENT,
    AWG_STREAM_UART_RAW_BAUD,
    AWG_STREAM_UART_RAW_BYTES_PER_S,
    build_awg_sweep_events,
    build_uniform_freq_list,
    estimate_uartlite_stream_seconds,
    pack_events,
    pack_stream_frame,
    parse_info_line,
    parse_last_artifact_block,
    parse_stream_ack_line,
    parse_stream_status_line,
    stream_frame_wire_stats,
)

try:
    import serial
except ImportError:  # pragma: no cover - environment-dependent
    serial = None

try:
    import pyvisa
except ImportError:  # pragma: no cover - environment-dependent
    pyvisa = None


BOOT_CHOICE_PROMPT = "choose an option [default 1]:"
CLOCK_CONFIG_PROMPT = "Clock configurations:"
BOOT_RATE_PROMPT = "Available sampling rates:"
NCO_START_PROMPT = "[NCO-TEST] Run 10 MHz DDS + AD9144 NCO discriminator test? [y/N]:"
CONTINUE_PROMPT = "Press ENTER to continue..."
NCO_DONE_MARKER = "[NCO-TEST] NCO disabled. Returning control to the DDS sweep."
DDS_BAND_START_PROMPT = "[DDS-BAND] Run focused DDS sweep diagnostic"
DDS_BAND_DONE_MARKER = "[DDS-BAND] Completed focused DDS band diagnostic. Returning to normal DDS sweep."
SFDR_START_PROMPT = "[SFDR-TEST] Run steady-state SFDR tone set"
SFDR_DONE_MARKER = "[SFDR-TEST] Completed steady-state SFDR tone set."
DYNAMIC_SFDR_START_PROMPT = "[DYNAMIC-SFDR] Run dynamic retune settling test? [y/N]:"
DYNAMIC_SFDR_DONE_MARKER = "[DYNAMIC-SFDR] Completed dynamic retune settling test."
THROUGHPUT_START_PROMPT = "[THROUGHPUT] Run MicroBlaze throughput benchmark? [y/N]:"
THROUGHPUT_DONE_MARKER = "[THROUGHPUT] Done."
UART_RTT_START_PROMPT = "[UART-RTT] Run host UART round-trip benchmark? [y/N]:"
UART_RTT_READY_MARKER = "[UART-RTT] Ready. Send 'PING <token>' and wait for 'PONG <token>'. Send DONE to exit."
UART_RTT_DONE_MARKER = "[UART-RTT] Done."
DDS_SWEEP_START_MARKER = "[DDS] AXI DAC core:"
AWG_SWEEP_START_PROMPT = "[AWG-SWEEP] Run AWG scheduler DDS sweep? [y/N]:"
AWG_CONSOLE_INFO_PREFIX = "[AWG-UART] INFO "
AWG_CONSOLE_BYE_MARKER = "[AWG-UART] Bye."
AWG_SET_EPOCH_ARTIFACT_PREFIX = "[SCHED-ARTIFACT] set_epoch before="
TRACE_MODE_TOKENS = {
    "write": "WRIT",
    "average": "AVER",
    "maxhold": "MAXH",
}
DETECTOR_TOKENS = {
    "positive": "POS",
    "sample": "SAMP",
    "rms": "RMS",
}
DEFAULT_DDS_SPAN_HZ = 10_000_000.0
DEFAULT_NCO_SPAN_HZ = 50_000_000.0
DEFAULT_DDS_SEARCH_MARGIN_HZ = 2_000_000.0
DEFAULT_NCO_SEARCH_MARGIN_HZ = 5_000_000.0
DEFAULT_SFDR_GUARD_HZ = 2_000_000.0
DEFAULT_SFDR_MIN_SEARCH_HZ = 5_000_000.0
DEFAULT_SFDR_MAX_SEARCH_HZ = 1_000_000_000.0
DEFAULT_DYNAMIC_INTENDED_MARGIN_HZ = 2_000_000.0
AWG_SWEEP_ANALYZER_DEFAULT_DWELL_US = 2_000_000
AWG_SWEEP_ANALYZER_MIN_DWELL_US = 250_000
AWG_SWEEP_ANALYZER_SETTLE_FRACTION = 0.05
AWG_SWEEP_ANALYZER_MIN_SETTLE_S = 0.005
AWG_SWEEP_ANALYZER_MAX_SETTLE_S = 0.050
AWG_SWEEP_MEASURE_MIN_SPAN_HZ = 500_000.0
AWG_SWEEP_MEASURE_MAX_SPAN_HZ = 1_000_000.0
DEFAULT_FREQ_SETTLE_TIMEOUT_S = 5.0
DEFAULT_FREQ_SETTLE_ERROR_HZ = 500_000.0
HOST_POLL_SLEEP_S = 0.01
BOOT_REPLY_PACING_S = 0.05
LEGACY_DDS_BAND_FREQS_HZ = [
    10_000_000.0,
    100_000_000.0,
    200_000_000.0,
    230_000_000.0,
    240_000_000.0,
    250_000_000.0,
    260_000_000.0,
    270_000_000.0,
    280_000_000.0,
    290_000_000.0,
    300_000_000.0,
    310_000_000.0,
    320_000_000.0,
    330_000_000.0,
]
LEGACY_SFDR_FREQS_HZ = [
    50_000_000.0,
    100_000_000.0,
    150_000_000.0,
    200_000_000.0,
    250_000_000.0,
    300_000_000.0,
    350_000_000.0,
    400_000_000.0,
]


@dataclass(frozen=True)
class StepSpec:
    group: str
    index: int
    name: str
    marker: str
    description: str
    expected_freq_hz: List[float]
    span_hz: float
    search_margin_hz: float

    @property
    def center_hz(self) -> float:
        return sum(self.expected_freq_hz) / float(len(self.expected_freq_hz))

    @property
    def search_left_hz(self) -> float:
        span_left = self.center_hz - (self.span_hz / 2.0)
        return max(span_left, min(self.expected_freq_hz) - self.search_margin_hz)

    @property
    def search_right_hz(self) -> float:
        span_right = self.center_hz + (self.span_hz / 2.0)
        return min(span_right, max(self.expected_freq_hz) + self.search_margin_hz)


@dataclass(frozen=True)
class AnalyzerSettings:
    rbw_hz: float
    vbw_hz: float
    sweep_count: int
    trace_mode: str
    detector: str
    reference_level_dbm: float
    display_range_db: float
    attenuation_auto: bool
    preamp_on: bool
    impedance_ohms: int
    capture_trace: bool


@dataclass(frozen=True)
class SfdrSettings:
    search_start_hz: float
    search_stop_hz: float
    carrier_guard_hz: float


@dataclass(frozen=True)
class PhaseNoiseRequest:
    carrier_hz: float
    span_hz: float
    step_spec: StepSpec


@dataclass(frozen=True)
class PhaseNoiseOffsetRequest:
    carrier_hz: float
    offset_hz: float
    sideband_window_hz: float
    step_spec: StepSpec


@dataclass(frozen=True)
class DynamicRetuneSpec:
    index: int
    name: str
    marker: str
    done_marker: str
    description: str
    intended_freq_hz: List[float]
    dwell_ms: int
    transitions: int
    intended_margin_hz: float


@dataclass(frozen=True)
class SweepRange:
    start_hz: float
    stop_hz: float
    step_hz: float


@dataclass
class WindowPeak:
    label: str
    search_left_hz: float
    search_right_hz: float
    power_dbm: Optional[float]
    freq_hz: Optional[float]


@dataclass
class DynamicRetuneMetrics:
    dwell_ms: int
    transitions: int
    active_duration_ms: int
    search_left_hz: float
    search_right_hz: float
    carrier_guard_hz: float
    intended_margin_hz: float
    rbw_hz: float
    vbw_hz: float
    sweep_count: int
    trace_mode: str
    detector: str
    reference_level_dbm: float
    display_range_db: float
    attenuation_auto: bool
    preamp_on: bool
    impedance_ohms: int
    intended_peaks: List[WindowPeak]
    unintended_peaks: List[WindowPeak]
    measured_elapsed_us: Optional[int]
    measured_us_per_transition: Optional[float]
    reference_power_dbm: Optional[float]
    reference_freq_hz: Optional[float]
    spur_power_dbm: Optional[float]
    spur_freq_hz: Optional[float]
    dynamic_spur_margin_db: Optional[float]


@dataclass
class PhaseNoiseOffsetMetrics:
    carrier_power_dbm: float
    carrier_freq_hz: float
    offset_hz: float
    sideband_window_hz: float
    rbw_hz: float
    vbw_hz: float
    sweep_count: int
    trace_mode: str
    detector: str
    reference_level_dbm: float
    display_range_db: float
    attenuation_auto: bool
    preamp_on: bool
    impedance_ohms: int
    sidebands: List[WindowPeak]
    avg_sideband_power_dbm: Optional[float]
    avg_sideband_dbc: Optional[float]
    avg_sideband_dbc_per_hz: Optional[float]


@dataclass
class SpectrumMetrics:
    trace_points: int
    center_hz: float
    span_hz: float
    search_left_hz: float
    search_right_hz: float
    rbw_hz: float
    vbw_hz: float
    sweep_count: int
    trace_mode: str
    detector: str
    reference_level_dbm: float
    display_range_db: float
    attenuation_auto: bool
    preamp_on: bool
    impedance_ohms: int
    power_dbm: float
    power_freq_hz: float
    marker_power_dbm: Optional[float]
    marker_freq_hz: Optional[float]
    trace_peak_power_dbm: float
    trace_peak_freq_hz: float
    nearest_expected_hz: Optional[float]
    nearest_error_hz: Optional[float]
    left_spur_power_dbm: Optional[float] = None
    left_spur_freq_hz: Optional[float] = None
    right_spur_power_dbm: Optional[float] = None
    right_spur_freq_hz: Optional[float] = None
    spur_power_dbm: Optional[float] = None
    spur_freq_hz: Optional[float] = None
    sfdr_db: Optional[float] = None
    reference_step_name: Optional[str] = None
    reference_power_dbm: Optional[float] = None
    power_delta_db: Optional[float] = None
    trace_capture_degraded: bool = False
    trace_capture_error: Optional[str] = None


@dataclass
class StepCaptureSummary:
    group: str
    step_index: int
    name: str
    marker: str
    description: str
    expected_freq_hz: List[float]
    csv_path: str
    metrics: Any


@dataclass(frozen=True)
class SchedulerBatchSpec:
    batch_index: int
    total_batches: int
    start_index: int
    freqs_hz: List[float]


NCO_STEP_SPECS = [
    StepSpec(
        group="nco",
        index=1,
        name="baseband_10mhz",
        marker="[NCO-TEST] Step 1/3: baseband only.",
        description="DDS complex baseband tone only",
        expected_freq_hz=[10_000_000.0],
        span_hz=DEFAULT_DDS_SPAN_HZ,
        search_margin_hz=DEFAULT_DDS_SEARCH_MARGIN_HZ,
    ),
    StepSpec(
        group="nco",
        index=2,
        name="nco_plus_300mhz",
        marker="[NCO-TEST] Step 2/3: applied +300 MHz NCO carrier.",
        description="DDS baseband shifted by +300 MHz",
        expected_freq_hz=[290_000_000.0, 310_000_000.0],
        span_hz=DEFAULT_NCO_SPAN_HZ,
        search_margin_hz=DEFAULT_NCO_SEARCH_MARGIN_HZ,
    ),
    StepSpec(
        group="nco",
        index=3,
        name="nco_minus_300mhz",
        marker="[NCO-TEST] Step 3/3: applied -300 MHz NCO carrier.",
        description="DDS baseband shifted by -300 MHz",
        expected_freq_hz=[290_000_000.0, 310_000_000.0],
        span_hz=DEFAULT_NCO_SPAN_HZ,
        search_margin_hz=DEFAULT_NCO_SEARCH_MARGIN_HZ,
    ),
]

def format_step_freq_label(freq_hz: float) -> str:
    mhz = freq_hz / 1_000_000.0
    if abs(mhz - round(mhz)) < 1e-9:
        return f"{int(round(mhz))} MHz"
    khz = freq_hz / 1_000.0
    if abs(khz - round(khz)) < 1e-6:
        return f"{int(round(khz))} kHz"
    return f"{freq_hz:.0f} Hz"


def format_step_freq_slug(freq_hz: float) -> str:
    khz = int(round(freq_hz / 1_000.0))
    if khz % 1000 == 0:
        return f"{khz // 1000}mhz"
    return f"{khz}khz"


def build_uniform_freq_list(start_hz: float, stop_hz: float, step_hz: float) -> List[float]:
    count = int(round((stop_hz - start_hz) / step_hz)) + 1
    return [start_hz + (idx * step_hz) for idx in range(count)]


def build_single_tone_step_specs(
    group: str,
    tag: str,
    freqs_hz: Sequence[float],
    description_prefix: str,
) -> List[StepSpec]:
    total = len(freqs_hz)
    specs: List[StepSpec] = []
    for index, freq_hz in enumerate(freqs_hz, start=1):
        freq_label = format_step_freq_label(freq_hz)
        freq_slug = format_step_freq_slug(freq_hz)
        specs.append(
            StepSpec(
                group=group,
                index=index,
                name=f"{group}_{freq_slug}",
                marker=f"[{tag}] Step {index}/{total}: {freq_label} DDS tone.",
                description=f"{description_prefix} at {freq_label}",
                expected_freq_hz=[freq_hz],
                span_hz=DEFAULT_DDS_SPAN_HZ,
                search_margin_hz=DEFAULT_DDS_SEARCH_MARGIN_HZ,
            )
        )
    return specs


def chunk_sequence(values: Sequence[float], chunk_size: int) -> List[List[float]]:
    if chunk_size <= 0:
        raise ValueError("chunk_size must be greater than 0")
    return [list(values[idx : idx + chunk_size]) for idx in range(0, len(values), chunk_size)]


def build_scheduler_batch_specs(freqs_hz: Sequence[float], max_events: int) -> List[SchedulerBatchSpec]:
    chunks = chunk_sequence(freqs_hz, max_events)
    specs: List[SchedulerBatchSpec] = []
    start_index = 0
    for batch_index, chunk in enumerate(chunks, start=1):
        specs.append(
            SchedulerBatchSpec(
                batch_index=batch_index,
                total_batches=len(chunks),
                start_index=start_index,
                freqs_hz=list(chunk),
            )
        )
        start_index += len(chunk)
    return specs


def build_scheduler_dense_step_specs(
    freqs_hz: Sequence[float],
    *,
    step_index_offset: int = 0,
    group: str = "scheduler_dense",
    description_prefix: str = "Scheduler dense sweep tone",
) -> List[StepSpec]:
    specs: List[StepSpec] = []
    for local_index, freq_hz in enumerate(freqs_hz, start=1):
        global_index = step_index_offset + local_index
        freq_label = format_step_freq_label(freq_hz)
        freq_slug = format_step_freq_slug(freq_hz)
        specs.append(
            StepSpec(
                group=group,
                index=global_index,
                name=f"{group}_{freq_slug}",
                marker="",
                description=f"{description_prefix} at {freq_label}",
                expected_freq_hz=[freq_hz],
                span_hz=min(
                    AWG_SWEEP_MEASURE_MAX_SPAN_HZ,
                    max(AWG_SWEEP_MEASURE_MIN_SPAN_HZ, abs(freq_hz) * 0.0 + 1_000_000.0),
                ),
                search_margin_hz=max(AWG_SWEEP_MEASURE_MIN_SPAN_HZ / 2.0, 250_000.0),
            )
        )
    return specs


def build_awg_scheduler_step_specs(freqs_hz: Sequence[float]) -> List[StepSpec]:
    specs = build_single_tone_step_specs(
        group="awg_scheduler",
        tag="AWG-SCHED",
        freqs_hz=freqs_hz,
        description_prefix="AWG scheduler DDS tone",
    )
    if len(freqs_hz) < 2:
        return specs

    sorted_freqs = sorted(float(item) for item in freqs_hz)
    min_spacing_hz = min(
        sorted_freqs[index + 1] - sorted_freqs[index]
        for index in range(len(sorted_freqs) - 1)
    )
    narrow_span_hz = max(
        AWG_SWEEP_MEASURE_MIN_SPAN_HZ,
        min(AWG_SWEEP_MEASURE_MAX_SPAN_HZ, min_spacing_hz * 0.5),
    )

    return [
        StepSpec(
            group=item.group,
            index=item.index,
            name=item.name,
            marker=item.marker,
            description=item.description,
            expected_freq_hz=item.expected_freq_hz,
            span_hz=narrow_span_hz,
            search_margin_hz=narrow_span_hz / 2.0,
        )
        for item in specs
    ]

DYNAMIC_RETUNE_STEP_SPECS = [
    DynamicRetuneSpec(
        index=1,
        name="dynamic_toggle_100_400_1ms",
        marker="[DYNAMIC-SFDR] Step 1/2: toggle_100_to_400_1ms.",
        done_marker="[DYNAMIC-SFDR] Completed burst 1/2.",
        description="Rapid DDS retune burst toggling 100 MHz <-> 400 MHz with 1 ms dwell",
        intended_freq_hz=[100_000_000.0, 400_000_000.0],
        dwell_ms=1,
        transitions=12_000,
        intended_margin_hz=DEFAULT_DYNAMIC_INTENDED_MARGIN_HZ,
    ),
    DynamicRetuneSpec(
        index=2,
        name="dynamic_toggle_100_400_10ms",
        marker="[DYNAMIC-SFDR] Step 2/2: toggle_100_to_400_10ms.",
        done_marker="[DYNAMIC-SFDR] Completed burst 2/2.",
        description="Rapid DDS retune burst toggling 100 MHz <-> 400 MHz with 10 ms dwell",
        intended_freq_hz=[100_000_000.0, 400_000_000.0],
        dwell_ms=10,
        transitions=1_200,
        intended_margin_hz=DEFAULT_DYNAMIC_INTENDED_MARGIN_HZ,
    ),
]

MAX_DYNAMIC_CASES = 8


def require_dependency(module, package_name: str) -> None:
    if module is None:
        raise SystemExit(
            f"Missing dependency: {package_name}. "
            f"Install it first, for example: python -m pip install {package_name}"
        )


def utc_timestamp() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def json_default(obj):
    if hasattr(obj, "__dataclass_fields__"):
        return asdict(obj)
    raise TypeError(f"Object of type {type(obj).__name__} is not JSON serializable")


class ConsoleLog:
    def __init__(self, file_path: Path):
        self.file_path = file_path
        self._fp = file_path.open("w", encoding="utf-8", newline="")

    def write(self, text: str) -> None:
        self._fp.write(text)
        self._fp.flush()
        sys.stdout.write(text)
        sys.stdout.flush()

    def close(self) -> None:
        self._fp.close()


class UartCoordinator:
    def __init__(
        self,
        port: str,
        baudrate: int,
        timeout_s: float,
        log: ConsoleLog,
        dtr: Optional[bool],
        rts: Optional[bool],
    ):
        require_dependency(serial, "pyserial")
        self.log = log
        self.port = port
        self.baudrate = baudrate
        self.timeout_s = timeout_s
        self.dtr = dtr
        self.rts = rts
        self.ser = self._open_serial()
        self._buffer = ""
        self.rx_count = 0

    def _open_serial(self):
        ser = serial.Serial(
            port=self.port,
            baudrate=self.baudrate,
            timeout=0.1,
            write_timeout=max(self.timeout_s, 1.0),
        )
        if self.dtr is not None:
            ser.dtr = self.dtr
        if self.rts is not None:
            ser.rts = self.rts
        return ser

    def close(self) -> None:
        if self.ser is not None and self.ser.is_open:
            self.ser.close()

    def reopen(self, retries: int = 20, retry_delay_s: float = 0.25) -> None:
        last_exc = None
        self.close()
        self._buffer = ""

        for _ in range(retries):
            try:
                self.ser = self._open_serial()
                return
            except serial.SerialException as exc:  # pragma: no cover - hardware-dependent
                last_exc = exc
                time.sleep(retry_delay_s)

        raise RuntimeError(f"Failed to reopen UART port {self.port}: {last_exc}")

    def send(self, text: str) -> None:
        payload = text.encode("ascii", errors="ignore")
        self.ser.write(payload)
        self.ser.flush()

    def send_bytes(self, payload: bytes) -> None:
        self.ser.write(payload)
        self.ser.flush()

    def send_line(self, text: str = "") -> None:
        self.send(text + "\r")

    def pump(self) -> bool:
        chunk = self.ser.read(self.ser.in_waiting or 1)
        if not chunk:
            return False

        text = chunk.decode("utf-8", errors="replace")
        self.log.write(text)
        self._buffer = (self._buffer + text)[-65536:]
        self.rx_count += len(chunk)
        return True

    def wait_for(
        self,
        needle: str,
        timeout_s: float,
        extra_needles: Optional[Iterable[str]] = None,
    ) -> str:
        deadline = time.monotonic() + timeout_s
        needles = [needle]
        if extra_needles:
            needles.extend(extra_needles)

        while time.monotonic() < deadline:
            matched = self._consume_match(needles)
            if matched is not None:
                return matched

            if not self.pump():
                time.sleep(HOST_POLL_SLEEP_S)

        raise TimeoutError(f"Timed out waiting for UART text: {needle!r}")

    def _consume_match(self, needles: Iterable[str]) -> Optional[str]:
        found: Optional[Tuple[int, str]] = None
        for item in needles:
            pos = self._buffer.find(item)
            if pos >= 0 and (found is None or pos < found[0]):
                found = (pos, item)

        if found is None:
            return None

        end = found[0] + len(found[1])
        self._buffer = self._buffer[end:]
        return found[1]

    def wait_for_line_containing(self, needle: str, timeout_s: float) -> str:
        line, _ = self.wait_for_line_containing_timed(needle, timeout_s)
        return line

    def wait_for_line_containing_timed(self, needle: str, timeout_s: float) -> Tuple[str, float]:
        deadline = time.monotonic() + timeout_s

        while time.monotonic() < deadline:
            matched = self._consume_line_containing(needle)
            if matched is not None:
                return matched, time.monotonic()

            if not self.pump():
                time.sleep(HOST_POLL_SLEEP_S)

        raise TimeoutError(f"Timed out waiting for UART line containing: {needle!r}")

    def _consume_line_containing(self, needle: str) -> Optional[str]:
        pos = self._buffer.find(needle)
        if pos < 0:
            return None

        line_start = max(self._buffer.rfind("\n", 0, pos), self._buffer.rfind("\r", 0, pos))
        if line_start < 0:
            line_start = 0
        else:
            line_start += 1

        line_end_candidates = [idx for idx in (self._buffer.find("\n", pos), self._buffer.find("\r", pos)) if idx >= 0]
        if not line_end_candidates:
            return None

        line_end = min(line_end_candidates)
        line = self._buffer[line_start:line_end]
        self._buffer = self._buffer[line_end + 1 :]
        return line.strip()


class RohdeSchwarzFSH:
    def __init__(
        self,
        resource_name: str,
        visa_backend: Optional[str],
        timeout_s: float,
    ):
        require_dependency(pyvisa, "pyvisa pyvisa-py")
        self.rm = None
        self.inst = None
        self.rm = pyvisa.ResourceManager(visa_backend) if visa_backend else pyvisa.ResourceManager()
        self.inst = self.rm.open_resource(resource_name)
        self.inst.timeout = int(timeout_s * 1000)
        self.inst.chunk_size = 1024 * 1024
        self.inst.write_termination = "\n"
        self.inst.read_termination = "\n"
        self.inst.write("*CLS")
        self.idn = self.inst.query("*IDN?").strip()
        self.last_io = "*IDN?"
        self.firmware_major = self._parse_firmware_major(self.idn)

    @staticmethod
    def _parse_firmware_major(idn: str) -> Optional[int]:
        match = re.search(r",V(\d+)(?:\.\d+)?$", idn.strip(), re.IGNORECASE)
        if not match:
            return None
        try:
            return int(match.group(1))
        except ValueError:
            return None

    @property
    def legacy_firmware(self) -> bool:
        return self.firmware_major is not None and self.firmware_major < 2

    def trace_capture_unsupported_message(self) -> str:
        return (
            f"Analyzer {self.idn} uses legacy firmware that rejects the trace-export "
            "SCPI required for span-based trace capture on this bench. "
            "Use --phase-noise-offset-hz for marker-only sideband measurements, "
            "leave --capture-trace disabled, or upgrade the analyzer firmware."
        )

    def apply_preset(self, preset_mode: str) -> None:
        if preset_mode == "off":
            return
        if preset_mode == "system":
            self._write("SYST:PRES")
        elif preset_mode == "reset":
            self._write("*RST")
        else:
            raise ValueError(f"Unsupported analyzer preset mode: {preset_mode}")
        # Give the analyzer a brief settle interval after a global preset/reset.
        time.sleep(1.0)
        self._write("*CLS")

    def _safe_query_text(self, command: str) -> dict:
        try:
            return {"ok": True, "value": self._query_text(command)}
        except Exception as exc:
            return {"ok": False, "error": str(exc)}

    def readback_state(self) -> dict:
        state = {
            "idn": self.idn,
            "firmware_major": self.firmware_major,
            "legacy_firmware": self.legacy_firmware,
            "queries": {},
            "system_errors": self.query_system_errors(),
        }
        for command in (
            "FREQ:CENT?",
            "FREQ:SPAN?",
            "BAND?",
            "BAND:VID?",
            "SWE:COUN?",
            "DET?",
            "DISP:TRAC:Y:RLEV?",
            "INP:ATT:AUTO?",
            "INP:GAIN:STAT?",
            "UNIT:POW?",
        ):
            state["queries"][command] = self._safe_query_text(command)
        return state

    def _wrap_io_error(self, exc: Exception) -> RuntimeError:
        return RuntimeError(f"Analyzer SCPI failed at '{self.last_io}': {exc}")

    def close(self) -> None:
        try:
            if self.inst is not None:
                self.inst.close()
        finally:
            if self.rm is not None:
                self.rm.close()

    @staticmethod
    def list_resources(visa_backend: Optional[str]) -> List[str]:
        require_dependency(pyvisa, "pyvisa pyvisa-py")
        rm = pyvisa.ResourceManager(visa_backend) if visa_backend else pyvisa.ResourceManager()
        try:
            return list(rm.list_resources())
        finally:
            rm.close()

    def _write(self, command: str) -> None:
        self.last_io = command
        try:
            self.inst.write(command)
        except Exception as exc:
            raise self._wrap_io_error(exc) from exc

    def _query_float(self, command: str) -> float:
        self.last_io = command
        try:
            return float(self.inst.query(command).strip())
        except Exception as exc:
            raise self._wrap_io_error(exc) from exc

    def _query_text(self, command: str) -> str:
        self.last_io = command
        try:
            return self.inst.query(command).strip()
        except Exception as exc:
            raise self._wrap_io_error(exc) from exc

    def query_system_errors(self) -> List[str]:
        errors: List[str] = []
        for command in ("SYST:ERR:ALL?", "SYST:ERR?"):
            try:
                response = self._query_text(command)
            except Exception as exc:
                errors.append(f"{command} failed: {exc}")
                continue
            if response:
                errors.append(f"{command} -> {response}")
        return errors

    def _query_ascii_float_list(self, command: str) -> List[float]:
        self.last_io = command
        try:
            raw = self.inst.query(command).strip()
        except Exception as exc:
            raise self._wrap_io_error(exc) from exc
        if not raw:
            return []
        return [float(item) for item in raw.split(",") if item.strip()]

    def _query_binary_float_list(self, command: str) -> List[float]:
        self.last_io = command
        previous_read_termination = self.inst.read_termination
        try:
            # R&S documents binary REAL,32 trace transfer as the faster path for
            # trace data. Disabling the text terminator avoids hanging on the
            # end-of-message if the transport does not deliver a trailing LF in
            # the way pyvisa.query() expects.
            self.inst.read_termination = None
            values = self.inst.query_binary_values(
                command,
                datatype="f",
                is_big_endian=False,
                container=list,
                header_fmt="ieee",
                expect_termination=False,
            )
        except Exception as exc:
            raise self._wrap_io_error(exc) from exc
        finally:
            self.inst.read_termination = previous_read_termination
        return [float(item) for item in values]

    def _read_raw_response(self, command: str, *, expect_binary: bool) -> bytes:
        self.last_io = command
        previous_read_termination = self.inst.read_termination
        chunks = bytearray()
        try:
            self.inst.read_termination = None
            self.inst.write(command)
            for _ in range(8):
                try:
                    chunk = self.inst.read_raw()
                except Exception as exc:
                    if chunks:
                        break
                    raise self._wrap_io_error(exc) from exc
                if not chunk:
                    break
                chunks.extend(chunk)
                if expect_binary:
                    if len(chunks) >= 2 and chunks[:1] == b"#":
                        digits = chunks[1] - ord("0")
                        if 0 <= digits <= 9 and len(chunks) >= 2 + digits:
                            payload_len = int(chunks[2 : 2 + digits].decode("ascii"))
                            total_len = 2 + digits + payload_len
                            if len(chunks) >= total_len:
                                break
                else:
                    if b"\n" in chunks or b"\r" in chunks or chunks.count(b",") >= 630:
                        break
        finally:
            self.inst.read_termination = previous_read_termination
        return bytes(chunks)

    @staticmethod
    def _parse_ieee_block_f32_le(raw: bytes) -> List[float]:
        if not raw.startswith(b"#") or len(raw) < 2:
            raise RuntimeError("Analyzer did not return an IEEE block")
        digits = raw[1] - ord("0")
        if digits < 0 or digits > 9:
            raise RuntimeError("Analyzer returned an invalid IEEE block header")
        if len(raw) < 2 + digits:
            raise RuntimeError("Analyzer IEEE block header is incomplete")
        payload_len = int(raw[2 : 2 + digits].decode("ascii"))
        start = 2 + digits
        end = start + payload_len
        if len(raw) < end:
            raise RuntimeError("Analyzer IEEE block payload is incomplete")
        payload = raw[start:end]
        if payload_len % 4 != 0:
            raise RuntimeError("Analyzer IEEE block payload length is not aligned to float32")
        count = payload_len // 4
        return list(struct.unpack("<" + ("f" * count), payload))

    def _query_binary_float_list_raw(self, command: str) -> List[float]:
        raw = self._read_raw_response(command, expect_binary=True)
        if not raw:
            return []
        return [float(item) for item in self._parse_ieee_block_f32_le(raw)]

    def _query_ascii_float_list_raw(self, command: str) -> List[float]:
        raw = self._read_raw_response(command, expect_binary=False)
        if not raw:
            return []
        text = raw.decode("ascii", errors="replace").strip().strip("\x00")
        if not text:
            return []
        return [float(item) for item in text.split(",") if item.strip()]

    def _capture_trace_data(
        self,
        center_hz: float,
        span_hz: float,
    ) -> Tuple[List[float], List[float]]:
        if self.legacy_firmware:
            raise RuntimeError(self.trace_capture_unsupported_message())

        attempts: List[str] = []
        trace_levels_dbm: List[float] = []

        trace_attempts = (
            ("FORM:BORD SWAP", None, "write"),
            ("FORM:DATA REAL,32", "TRAC:DATA? TRACE1", "binary_raw"),
            ("FORM:DATA REAL,32", "TRAC? TRACE1", "binary_raw"),
            ("FORM:DATA REAL,32", "TRAC:DATA:MEM? TRACE1", "binary_mem_raw"),
            ("FORM:DATA ASC", "TRAC:DATA? TRACE1", "ascii_raw"),
            ("FORM:DATA ASC", "TRAC? TRACE1", "ascii_raw"),
            ("FORM:DATA ASC", "TRAC:DATA:MEM? TRACE1", "ascii_mem_raw"),
            ("FORM:DATA REAL,32", "TRAC:DATA? TRACE1", "binary"),
            ("FORM:DATA REAL,32", "TRAC? TRACE1", "binary"),
            ("FORM:DATA REAL,32", "TRAC:DATA:MEM? TRACE1", "binary_mem"),
            ("FORM:DATA ASC", "TRAC:DATA? TRACE1", "ascii"),
            ("FORM:DATA ASC", "TRAC? TRACE1", "ascii"),
            ("FORM:DATA ASC", "TRAC:DATA:MEM? TRACE1", "ascii_mem"),
        )

        for format_command, query_command, mode in trace_attempts:
            try:
                if format_command is not None:
                    self._write(format_command)
                if mode == "write":
                    continue
                if "_mem" in mode:
                    self._write("CALC:MATH:COPY:MEM")
                    self._write("DISP:TRAC:MEM ON")
                if mode == "binary_raw" or mode == "binary_mem_raw":
                    trace_levels_dbm = self._query_binary_float_list_raw(query_command)
                elif mode == "ascii_raw" or mode == "ascii_mem_raw" or mode == "ascii_raw_legacy":
                    trace_levels_dbm = self._query_ascii_float_list_raw(query_command)
                elif mode == "binary" or mode == "binary_mem":
                    trace_levels_dbm = self._query_binary_float_list(query_command)
                else:
                    trace_levels_dbm = self._query_ascii_float_list(query_command)
                if trace_levels_dbm:
                    break
            except Exception as exc:
                prefix = format_command if format_command is not None else "legacy-default"
                attempts.append(f"{prefix} + {query_command}: {exc}")
                trace_levels_dbm = []

        if not trace_levels_dbm:
            system_errors = self.query_system_errors()
            detail = "; ".join(attempts) if attempts else "Analyzer returned no usable trace points"
            if system_errors:
                detail += "; " + "; ".join(system_errors)
            raise RuntimeError(detail)
        trace_freqs_hz = build_trace_axis(center_hz, span_hz, len(trace_levels_dbm))
        return trace_freqs_hz, trace_levels_dbm

    def _configure_spectrum_measurement(
        self,
        center_hz: float,
        span_hz: float,
        settings: AnalyzerSettings,
    ) -> None:
        self._write("*CLS")
        # FW 1.58 rejects several newer display / format / auto-detector
        # headers. Keep the legacy path on the small set that the compatibility
        # probe showed to be accepted.
        if not self.legacy_firmware:
            self._write("UNIT:POW DBM")
            self._write("DISP:TRAC:Y:SPAC LOG")
            self._write(f"DISP:TRAC:Y {settings.display_range_db}dB")
            self._write(f"INP:IMP {settings.impedance_ohms}")
            self._write("DET:AUTO OFF")
            self._write("CALC:MARK1:FREQ:MODE FREQ")
        self._write(f"DISP:TRAC:Y:RLEV {settings.reference_level_dbm}dBm")
        self._write(f"INP:ATT:AUTO {'ON' if settings.attenuation_auto else 'OFF'}")
        self._write(f"INP:GAIN:STAT {'ON' if settings.preamp_on else 'OFF'}")
        self._write("BAND:AUTO OFF")
        self._write(f"BAND {settings.rbw_hz}")
        self._write("BAND:VID:AUTO OFF")
        self._write(f"BAND:VID {settings.vbw_hz}")
        self._write("SWE:TIME:AUTO ON")
        self._write("INIT:CONT OFF")
        self._write(f"SWE:COUN {settings.sweep_count}")
        self._write(f"DISP:WIND:TRAC:MODE {TRACE_MODE_TOKENS[settings.trace_mode]}")
        self._write(f"DET {DETECTOR_TOKENS[settings.detector]}")
        self._write(f"FREQ:CENT {center_hz}")
        self._write(f"FREQ:SPAN {span_hz}")
        self._write("CALC:MARK1 ON")
        self._write("INIT;*WAI")

    def _capture_peak_for_span(
        self,
        center_hz: float,
        span_hz: float,
        settings: AnalyzerSettings,
    ) -> Tuple[Optional[float], Optional[float]]:
        if span_hz <= 0.0:
            return None, None

        self._configure_spectrum_measurement(center_hz, span_hz, settings)
        self._write("CALC:MARK1:MAX")
        power_dbm = self._query_float("CALC:MARK1:Y?")
        freq_hz = self._query_float("CALC:MARK1:X?")
        return power_dbm, freq_hz

    def _query_marker_in_window(
        self,
        left_hz: float,
        right_hz: float,
    ) -> Tuple[Optional[float], Optional[float]]:
        if right_hz <= left_hz:
            return None, None

        self._write("CALC:MARK1:X:SLIM ON")
        self._write(f"CALC:MARK1:X:SLIM:LEFT {left_hz}")
        self._write(f"CALC:MARK1:X:SLIM:RIGH {right_hz}")
        self._write("CALC:MARK1:MAX")
        power_dbm = self._query_float("CALC:MARK1:Y?")
        try:
            freq_hz = self._query_float("CALC:MARK1:X?")
        except Exception:
            freq_hz = None
        return power_dbm, freq_hz

    def _capture_marker_at_frequency(
        self,
        center_hz: float,
        span_hz: float,
        marker_hz: float,
        settings: AnalyzerSettings,
    ) -> Tuple[Optional[float], Optional[float]]:
        if span_hz <= 0.0:
            return None, None

        self._configure_spectrum_measurement(center_hz, span_hz, settings)
        self._write(f"CALC:MARK1:X {marker_hz}")
        power_dbm = self._query_float("CALC:MARK1:Y?")
        try:
            freq_hz = self._query_float("CALC:MARK1:X?")
        except Exception:
            freq_hz = marker_hz
        return power_dbm, freq_hz

    def capture_trace(
        self,
        step: StepSpec,
        settings: AnalyzerSettings,
    ) -> Tuple[List[float], List[float], SpectrumMetrics]:
        if settings.capture_trace and self.legacy_firmware:
            raise RuntimeError(self.trace_capture_unsupported_message())

        center_hz = step.center_hz
        span_hz = step.span_hz
        search_left_hz = step.search_left_hz
        search_right_hz = step.search_right_hz

        self._configure_spectrum_measurement(center_hz, span_hz, settings)
        self._write("CALC:MARK1:X:SLIM ON")
        self._write(f"CALC:MARK1:X:SLIM:LEFT {search_left_hz}")
        self._write(f"CALC:MARK1:X:SLIM:RIGH {search_right_hz}")
        self._write("CALC:MARK1:MAX")

        marker_power_dbm = self._query_float("CALC:MARK1:Y?")
        marker_freq_hz = None
        try:
            marker_freq_hz = self._query_float("CALC:MARK1:X?")
        except Exception:
            marker_freq_hz = None

        trace_levels_dbm: List[float]
        sweep_points: int
        trace_freqs_hz: List[float]
        trace_capture_degraded = False
        trace_capture_error = None
        if settings.capture_trace:
            try:
                trace_freqs_hz, trace_levels_dbm = self._capture_trace_data(center_hz, span_hz)
                sweep_points = len(trace_levels_dbm)
            except Exception as exc:
                fallback_freq_hz = marker_freq_hz if marker_freq_hz is not None else center_hz
                trace_freqs_hz = [fallback_freq_hz]
                trace_levels_dbm = [marker_power_dbm]
                sweep_points = 1
                trace_capture_degraded = True
                trace_capture_error = str(exc)
                print(f"[HOST] Analyzer trace fallback after '{self.last_io}': {exc}")
        else:
            fallback_freq_hz = marker_freq_hz if marker_freq_hz is not None else center_hz
            trace_freqs_hz = [fallback_freq_hz]
            trace_levels_dbm = [marker_power_dbm]
            sweep_points = 1

        peak_index = max(range(len(trace_levels_dbm)), key=lambda idx: trace_levels_dbm[idx])
        trace_peak_power_dbm = trace_levels_dbm[peak_index]
        trace_peak_freq_hz = trace_freqs_hz[peak_index]

        power_dbm = marker_power_dbm if marker_power_dbm is not None else trace_peak_power_dbm
        power_freq_hz = marker_freq_hz if marker_freq_hz is not None else trace_peak_freq_hz
        nearest_expected_hz = None
        nearest_error_hz = None
        if step.expected_freq_hz:
            nearest_expected_hz = min(step.expected_freq_hz, key=lambda item: abs(item - power_freq_hz))
            nearest_error_hz = power_freq_hz - nearest_expected_hz

        metrics = SpectrumMetrics(
            trace_points=sweep_points,
            center_hz=center_hz,
            span_hz=span_hz,
            search_left_hz=search_left_hz,
            search_right_hz=search_right_hz,
            rbw_hz=settings.rbw_hz,
            vbw_hz=settings.vbw_hz,
            sweep_count=settings.sweep_count,
            trace_mode=settings.trace_mode,
            detector=settings.detector,
            reference_level_dbm=settings.reference_level_dbm,
            display_range_db=settings.display_range_db,
            attenuation_auto=settings.attenuation_auto,
            preamp_on=settings.preamp_on,
            impedance_ohms=settings.impedance_ohms,
            power_dbm=power_dbm,
            power_freq_hz=power_freq_hz,
            marker_power_dbm=marker_power_dbm,
            marker_freq_hz=marker_freq_hz,
            trace_peak_power_dbm=trace_peak_power_dbm,
            trace_peak_freq_hz=trace_peak_freq_hz,
            nearest_expected_hz=nearest_expected_hz,
            nearest_error_hz=nearest_error_hz,
            trace_capture_degraded=trace_capture_degraded,
            trace_capture_error=trace_capture_error,
        )
        return trace_freqs_hz, trace_levels_dbm, metrics

    def capture_known_tone(
        self,
        step: StepSpec,
        settings: AnalyzerSettings,
        *,
        lock_to_expected: bool = False,
    ) -> Tuple[List[float], List[float], SpectrumMetrics]:
        expected_hz = step.expected_freq_hz[0]
        center_hz = expected_hz
        span_hz = step.span_hz
        search_left_hz = center_hz - (span_hz / 2.0)
        search_right_hz = center_hz + (span_hz / 2.0)

        self._configure_spectrum_measurement(center_hz, span_hz, settings)
        self._write(f"CALC:MARK1:X {expected_hz}")
        marker_power_dbm = self._query_float("CALC:MARK1:Y?")
        try:
            marker_freq_hz = self._query_float("CALC:MARK1:X?")
        except Exception:
            marker_freq_hz = expected_hz

        if lock_to_expected:
            peak_power_dbm = marker_power_dbm
            peak_freq_hz = marker_freq_hz
        else:
            self._write("CALC:MARK1:MAX")
            peak_power_dbm = self._query_float("CALC:MARK1:Y?")
            peak_freq_hz = self._query_float("CALC:MARK1:X?")

        trace_capture_degraded = False
        trace_capture_error = None
        if settings.capture_trace:
            try:
                trace_freqs_hz, trace_levels_dbm = self._capture_trace_data(center_hz, span_hz)
            except Exception as exc:
                trace_capture_degraded = True
                trace_capture_error = str(exc)
                print(f"[HOST] Analyzer trace fallback after '{self.last_io}': {exc}")
                trace_freqs_hz = [peak_freq_hz]
                trace_levels_dbm = [peak_power_dbm]
        else:
            trace_freqs_hz = [peak_freq_hz]
            trace_levels_dbm = [peak_power_dbm]

        peak_index = max(range(len(trace_levels_dbm)), key=lambda idx: trace_levels_dbm[idx])
        trace_peak_power_dbm = trace_levels_dbm[peak_index]
        trace_peak_freq_hz = trace_freqs_hz[peak_index]

        metrics = SpectrumMetrics(
            trace_points=len(trace_levels_dbm),
            center_hz=center_hz,
            span_hz=span_hz,
            search_left_hz=search_left_hz,
            search_right_hz=search_right_hz,
            rbw_hz=settings.rbw_hz,
            vbw_hz=settings.vbw_hz,
            sweep_count=settings.sweep_count,
            trace_mode=settings.trace_mode,
            detector=settings.detector,
            reference_level_dbm=settings.reference_level_dbm,
            display_range_db=settings.display_range_db,
            attenuation_auto=settings.attenuation_auto,
            preamp_on=settings.preamp_on,
            impedance_ohms=settings.impedance_ohms,
            power_dbm=marker_power_dbm if lock_to_expected else peak_power_dbm,
            power_freq_hz=marker_freq_hz if lock_to_expected else peak_freq_hz,
            marker_power_dbm=marker_power_dbm,
            marker_freq_hz=marker_freq_hz,
            trace_peak_power_dbm=trace_peak_power_dbm,
            trace_peak_freq_hz=trace_peak_freq_hz,
            nearest_expected_hz=expected_hz,
            nearest_error_hz=(marker_freq_hz if lock_to_expected else peak_freq_hz) - expected_hz,
            trace_capture_degraded=trace_capture_degraded,
            trace_capture_error=trace_capture_error,
        )
        return trace_freqs_hz, trace_levels_dbm, metrics

    def capture_sfdr(
        self,
        step: StepSpec,
        settings: AnalyzerSettings,
        sfdr: SfdrSettings,
    ) -> SpectrumMetrics:
        carrier_hz = step.expected_freq_hz[0]
        search_start_hz = max(sfdr.search_start_hz, 0.0)
        search_stop_hz = max(search_start_hz + 1.0, sfdr.search_stop_hz)
        center_hz = (search_start_hz + search_stop_hz) / 2.0
        span_hz = search_stop_hz - search_start_hz
        carrier_left_hz = max(search_start_hz, carrier_hz - step.search_margin_hz)
        carrier_right_hz = min(search_stop_hz, carrier_hz + step.search_margin_hz)

        carrier_center_hz = (carrier_left_hz + carrier_right_hz) / 2.0
        carrier_span_hz = max(carrier_right_hz - carrier_left_hz, 1.0)
        carrier_power_dbm, carrier_freq_hz = self._capture_peak_for_span(
            carrier_center_hz,
            carrier_span_hz,
            settings,
        )
        if carrier_power_dbm is None or carrier_freq_hz is None:
            raise RuntimeError(f"Could not locate SFDR carrier for {step.name}")

        left_spur_stop_hz = carrier_freq_hz - sfdr.carrier_guard_hz
        right_spur_start_hz = carrier_freq_hz + sfdr.carrier_guard_hz

        left_spur_power_dbm = None
        left_spur_freq_hz = None
        if left_spur_stop_hz > search_start_hz:
            left_center_hz = (search_start_hz + left_spur_stop_hz) / 2.0
            left_span_hz = left_spur_stop_hz - search_start_hz
            left_spur_power_dbm, left_spur_freq_hz = self._capture_peak_for_span(
                left_center_hz,
                left_span_hz,
                settings,
            )

        right_spur_power_dbm = None
        right_spur_freq_hz = None
        if search_stop_hz > right_spur_start_hz:
            right_center_hz = (right_spur_start_hz + search_stop_hz) / 2.0
            right_span_hz = search_stop_hz - right_spur_start_hz
            right_spur_power_dbm, right_spur_freq_hz = self._capture_peak_for_span(
                right_center_hz,
                right_span_hz,
                settings,
            )

        spur_candidates = [
            (left_spur_power_dbm, left_spur_freq_hz),
            (right_spur_power_dbm, right_spur_freq_hz),
        ]
        spur_power_dbm = None
        spur_freq_hz = None
        for power_dbm, freq_hz in spur_candidates:
            if power_dbm is None or freq_hz is None:
                continue
            if spur_power_dbm is None or power_dbm > spur_power_dbm:
                spur_power_dbm = power_dbm
                spur_freq_hz = freq_hz

        sfdr_db = None
        if spur_power_dbm is not None:
            sfdr_db = carrier_power_dbm - spur_power_dbm

        metrics = SpectrumMetrics(
            trace_points=0,
            center_hz=center_hz,
            span_hz=span_hz,
            search_left_hz=search_start_hz,
            search_right_hz=search_stop_hz,
            rbw_hz=settings.rbw_hz,
            vbw_hz=settings.vbw_hz,
            sweep_count=settings.sweep_count,
            trace_mode=settings.trace_mode,
            detector=settings.detector,
            reference_level_dbm=settings.reference_level_dbm,
            display_range_db=settings.display_range_db,
            attenuation_auto=settings.attenuation_auto,
            preamp_on=settings.preamp_on,
            impedance_ohms=settings.impedance_ohms,
            power_dbm=carrier_power_dbm,
            power_freq_hz=carrier_freq_hz,
            marker_power_dbm=carrier_power_dbm,
            marker_freq_hz=carrier_freq_hz,
            trace_peak_power_dbm=carrier_power_dbm,
            trace_peak_freq_hz=carrier_freq_hz,
            nearest_expected_hz=carrier_hz,
            nearest_error_hz=carrier_freq_hz - carrier_hz,
            left_spur_power_dbm=left_spur_power_dbm,
            left_spur_freq_hz=left_spur_freq_hz,
            right_spur_power_dbm=right_spur_power_dbm,
            right_spur_freq_hz=right_spur_freq_hz,
            spur_power_dbm=spur_power_dbm,
            spur_freq_hz=spur_freq_hz,
            sfdr_db=sfdr_db,
        )
        return metrics


def build_trace_axis(center_hz: float, span_hz: float, points: int) -> List[float]:
    if points <= 1:
        return [center_hz]
    start_hz = center_hz - (span_hz / 2.0)
    step_hz = span_hz / float(points - 1)
    return [start_hz + (index * step_hz) for index in range(points)]


def save_trace_csv(path: Path, freqs_hz: Sequence[float], levels_dbm: Sequence[float]) -> None:
    with path.open("w", encoding="utf-8", newline="") as fp:
        writer = csv.writer(fp)
        writer.writerow(["frequency_hz", "level_dbm"])
        for freq_hz, level_dbm in zip(freqs_hz, levels_dbm):
            writer.writerow([f"{freq_hz:.6f}", f"{level_dbm:.6f}"])


def print_step_summary(step: StepSpec, metrics: SpectrumMetrics) -> None:
    freq_text = f"{metrics.power_freq_hz / 1e6:.6f} MHz"
    err_text = ""
    delta_text = ""
    marker_text = ""
    spur_text = ""

    if metrics.nearest_expected_hz is not None and metrics.nearest_error_hz is not None:
        err_text = (
            f", nearest expected {metrics.nearest_expected_hz / 1e6:.6f} MHz"
            f" (error {metrics.nearest_error_hz / 1e6:+.6f} MHz)"
        )
    if metrics.power_delta_db is not None and metrics.reference_step_name is not None:
        delta_text = (
            f", PowerDelta={metrics.power_delta_db:+.3f} dB"
            f" vs {metrics.reference_step_name}"
        )
    if metrics.marker_freq_hz is not None:
        marker_text = (
            f", Marker={metrics.marker_power_dbm:.3f} dBm @ "
            f"{metrics.marker_freq_hz / 1e6:.6f} MHz"
        )
    if metrics.sfdr_db is not None and metrics.spur_freq_hz is not None:
        spur_text = (
            f", Spur={metrics.spur_power_dbm:.3f} dBm @ "
            f"{metrics.spur_freq_hz / 1e6:.6f} MHz"
            f", SFDR={metrics.sfdr_db:.3f} dBc"
        )

    print(
        f"[HOST] {step.name}: Power={metrics.power_dbm:.3f} dBm, "
        f"Freq={freq_text}{err_text}{delta_text}{marker_text}{spur_text}"
    )


def format_frequency_label_hz(freq_hz: float) -> str:
    rounded_hz = int(round(freq_hz))
    units = (
        (1_000_000_000, "ghz"),
        (1_000_000, "mhz"),
        (1_000, "khz"),
    )
    for scale, suffix in units:
        if rounded_hz % scale == 0 and rounded_hz >= scale:
            return f"{rounded_hz // scale}{suffix}"
    return f"{rounded_hz}hz"


def capture_with_frequency_settle(
    capture_fn: Any,
    step: StepSpec,
    *,
    timeout_s: float,
    error_hz: float,
) -> Tuple[List[float], List[float], SpectrumMetrics]:
    trace_freqs_hz, trace_levels_dbm, metrics = capture_fn()
    best_trace_freqs_hz = trace_freqs_hz
    best_trace_levels_dbm = trace_levels_dbm
    best_metrics = metrics
    best_error_hz = abs(metrics.nearest_error_hz) if metrics.nearest_error_hz is not None else None

    if timeout_s <= 0.0 or error_hz <= 0.0 or metrics.nearest_error_hz is None:
        return best_trace_freqs_hz, best_trace_levels_dbm, best_metrics

    deadline = time.monotonic() + timeout_s
    attempts = 1
    while abs(best_metrics.nearest_error_hz) > error_hz and time.monotonic() < deadline:
        trace_freqs_hz, trace_levels_dbm, metrics = capture_fn()
        attempts += 1
        current_error_hz = abs(metrics.nearest_error_hz) if metrics.nearest_error_hz is not None else None
        if current_error_hz is not None and (
            best_error_hz is None or current_error_hz < best_error_hz
        ):
            best_trace_freqs_hz = trace_freqs_hz
            best_trace_levels_dbm = trace_levels_dbm
            best_metrics = metrics
            best_error_hz = current_error_hz

    if attempts > 1:
        if best_metrics.nearest_error_hz is not None and abs(best_metrics.nearest_error_hz) <= error_hz:
            print(
                f"[HOST] {step.name}: settled after {attempts} capture(s), "
                f"best error {best_metrics.nearest_error_hz / 1e6:+.6f} MHz"
            )
        else:
            error_text = "n/a"
            if best_metrics.nearest_error_hz is not None:
                error_text = f"{best_metrics.nearest_error_hz / 1e6:+.6f} MHz"
            print(
                f"[HOST] {step.name}: settle timeout after {attempts} capture(s); "
                f"best error {error_text}"
            )

    return best_trace_freqs_hz, best_trace_levels_dbm, best_metrics


def capture_trace_step(
    analyzer: RohdeSchwarzFSH,
    output_dir: Path,
    step: StepSpec,
    settings: AnalyzerSettings,
    dump_analyzer_state: bool = False,
    write_csv: bool = False,
    write_json: bool = True,
    settle_timeout_s: float = 0.0,
    settle_error_hz: float = 0.0,
) -> StepCaptureSummary:
    trace_freqs_hz, trace_levels_dbm, metrics = capture_with_frequency_settle(
        lambda: analyzer.capture_trace(step, settings),
        step,
        timeout_s=settle_timeout_s,
        error_hz=settle_error_hz,
    )

    csv_path = output_dir / f"step{step.index:02d}_{step.name}.csv"
    json_path = output_dir / f"step{step.index:02d}_{step.name}.json"
    if write_csv:
        save_trace_csv(csv_path, trace_freqs_hz, trace_levels_dbm)
    if write_json:
        write_step_json(
            json_path,
            analyzer.idn,
            step,
            metrics,
            extra=build_step_extra(analyzer, dump_analyzer_state),
        )

    summary = StepCaptureSummary(
        group=step.group,
        step_index=step.index,
        name=step.name,
        marker=step.marker,
        description=step.description,
        expected_freq_hz=step.expected_freq_hz,
        csv_path=str(csv_path.resolve()) if write_csv else "",
        metrics=metrics,
    )
    return summary


def capture_known_tone_step(
    analyzer: RohdeSchwarzFSH,
    output_dir: Path,
    step: StepSpec,
    settings: AnalyzerSettings,
    dump_analyzer_state: bool = False,
    write_csv: bool = False,
    write_json: bool = True,
    settle_timeout_s: float = 0.0,
    settle_error_hz: float = 0.0,
    lock_to_expected: bool = False,
) -> StepCaptureSummary:
    trace_freqs_hz, trace_levels_dbm, metrics = capture_with_frequency_settle(
        lambda: analyzer.capture_known_tone(
            step,
            settings,
            lock_to_expected=lock_to_expected,
        ),
        step,
        timeout_s=settle_timeout_s,
        error_hz=settle_error_hz,
    )

    csv_path = output_dir / f"step{step.index:02d}_{step.name}.csv"
    json_path = output_dir / f"step{step.index:02d}_{step.name}.json"
    if write_csv:
        save_trace_csv(csv_path, trace_freqs_hz, trace_levels_dbm)
    if write_json:
        write_step_json(
            json_path,
            analyzer.idn,
            step,
            metrics,
            extra=build_step_extra(analyzer, dump_analyzer_state),
        )

    return StepCaptureSummary(
        group=step.group,
        step_index=step.index,
        name=step.name,
        marker=step.marker,
        description=step.description,
        expected_freq_hz=step.expected_freq_hz,
        csv_path=str(csv_path.resolve()) if write_csv else "",
        metrics=metrics,
    )


def write_step_json(
    path: Path,
    analyzer_idn: str,
    step: StepSpec,
    metrics: Any,
    extra: Optional[dict] = None,
) -> None:
    payload = {
        "group": step.group,
        "step_index": step.index,
        "name": step.name,
        "marker": step.marker,
        "description": step.description,
        "expected_freq_hz": step.expected_freq_hz,
        "metrics": asdict(metrics),
        "analyzer_idn": analyzer_idn,
    }
    if extra:
        payload.update(extra)
    path.write_text(
        json.dumps(payload, indent=2, default=json_default) + "\n",
        encoding="utf-8",
    )


def build_step_extra(
    analyzer: RohdeSchwarzFSH,
    dump_analyzer_state: bool,
    extra: Optional[dict] = None,
) -> dict:
    payload = dict(extra or {})
    if dump_analyzer_state:
        payload["analyzer_state"] = analyzer.readback_state()
    return payload


def build_awg_scheduler_analyzer_settings(base: AnalyzerSettings) -> AnalyzerSettings:
    if base.capture_trace:
        return base

    return AnalyzerSettings(
        rbw_hz=base.rbw_hz,
        vbw_hz=base.vbw_hz,
        sweep_count=1,
        trace_mode="write",
        detector=base.detector,
        reference_level_dbm=base.reference_level_dbm,
        display_range_db=base.display_range_db,
        attenuation_auto=base.attenuation_auto,
        preamp_on=base.preamp_on,
        impedance_ohms=base.impedance_ohms,
        capture_trace=False,
    )


def resolve_awg_sweep_dwell_us(args: argparse.Namespace, *, analyzer_enabled: bool) -> int:
    if args.awg_sweep_dwell_us is not None:
        dwell_us = args.awg_sweep_dwell_us
    elif analyzer_enabled:
        dwell_us = AWG_SWEEP_ANALYZER_DEFAULT_DWELL_US
    else:
        dwell_us = 1000

    if analyzer_enabled and dwell_us < AWG_SWEEP_ANALYZER_MIN_DWELL_US:
        raise RuntimeError(
            "AWG sweep analyzer validation requires a longer dwell window. "
            f"Use --awg-sweep-dwell-us >= {AWG_SWEEP_ANALYZER_MIN_DWELL_US}."
        )

    return dwell_us


def load_awg_scheduler_events_into_console(
    uart: "UartCoordinator",
    args: argparse.Namespace,
    events: Sequence["AwgSchedEvent"],
) -> dict:
    payload = pack_events(events)
    payload_hex = payload.hex()

    uart.send_line(f"LOADBIN {len(events)}")
    matched = uart.wait_for(
        AWG_CONSOLE_LOAD_READY_MARKER,
        args.uart_timeout,
        extra_needles=[AWG_CONSOLE_ERROR_MARKER],
    )
    if matched == AWG_CONSOLE_ERROR_MARKER:
        raise RuntimeError("Scheduler console reported an error before accepting LOADBIN payload.")

    matched = uart.wait_for(
        AWG_CONSOLE_LOAD_RX_BEGIN_MARKER,
        args.uart_timeout,
        extra_needles=[AWG_CONSOLE_ERROR_MARKER],
    )
    if matched == AWG_CONSOLE_ERROR_MARKER:
        raise RuntimeError("Scheduler console reported an error before entering LOADBIN receive mode.")

    for offset in range(0, len(payload_hex), 64):
        uart.send(payload_hex[offset : offset + 64])
        time.sleep(0.002)
    uart.send_line()

    matched = uart.wait_for(
        AWG_CONSOLE_LOAD_OK_MARKER,
        args.uart_timeout,
        extra_needles=[AWG_CONSOLE_ERROR_MARKER],
    )
    if matched == AWG_CONSOLE_ERROR_MARKER:
        raise RuntimeError("Scheduler console reported an error during LOADBIN.")

    return {
        "event_count": len(events),
        "event_payload_bytes": len(payload),
        "event_payload_hex_chars": len(payload_hex),
    }


def query_awg_stream_status(
    uart: "UartCoordinator",
    args: argparse.Namespace,
    *,
    command: str = "STREAMSTATUS",
) -> Any:
    uart.send_line(command)
    line = uart.wait_for_line_containing(AWG_STREAM_STATUS_PREFIX, args.uart_timeout)
    return parse_stream_status_line(line)


def reset_awg_stream_console(uart: "UartCoordinator", args: argparse.Namespace) -> Any:
    uart.send_line("STREAMRESET")
    line = uart.wait_for_line_containing(AWG_STREAM_STATUS_PREFIX, args.uart_timeout)
    return parse_stream_status_line(line)


def send_awg_stream_frame_hex(
    uart: "UartCoordinator",
    args: argparse.Namespace,
    frame: bytes,
    *,
    expected_status: Optional[int] = 0,
) -> dict:
    payload_hex = frame.hex()
    command_start_s = time.monotonic()

    uart.send_line(f"STREAMHEX {len(frame)}")
    matched = uart.wait_for(
        AWG_STREAM_READY_MARKER,
        args.uart_timeout,
        extra_needles=[AWG_STREAM_ERROR_MARKER, AWG_CONSOLE_ERROR_MARKER],
    )
    if matched != AWG_STREAM_READY_MARKER:
        raise RuntimeError(f"Stream console rejected frame before payload: {matched}")

    matched = uart.wait_for(
        AWG_STREAM_RX_BEGIN_MARKER,
        args.uart_timeout,
        extra_needles=[AWG_STREAM_ERROR_MARKER, AWG_CONSOLE_ERROR_MARKER],
    )
    if matched != AWG_STREAM_RX_BEGIN_MARKER:
        raise RuntimeError(f"Stream console rejected frame before RX: {matched}")

    chunk_chars = max(2, int(args.scheduler_stream_hex_line_chars))
    if chunk_chars % 2:
        chunk_chars -= 1
    for offset in range(0, len(payload_hex), chunk_chars):
        uart.send(payload_hex[offset : offset + chunk_chars])
        if args.scheduler_stream_hex_chunk_delay_s > 0:
            time.sleep(args.scheduler_stream_hex_chunk_delay_s)
    uart.send_line()

    ack_line, ack_time_s = uart.wait_for_line_containing_timed(
        AWG_STREAM_ACK_PREFIX,
        args.uart_timeout,
    )
    ack = parse_stream_ack_line(ack_line)
    elapsed_s = max(0.0, ack_time_s - command_start_s)
    stats = stream_frame_wire_stats(frame, elapsed_s=elapsed_s)
    event_count = max(0, ack.event_count)
    if elapsed_s > 0 and event_count > 0:
        stats["wall_clock_per_event_s"] = elapsed_s / event_count
        stats["effective_events_per_s"] = event_count / elapsed_s
    stats["ack_latency_s"] = elapsed_s

    if ack.magic != AWG_STREAM_PROTO_MAGIC:
        raise RuntimeError(f"Stream ACK magic mismatch: 0x{ack.magic:08X}")
    if expected_status is not None and ack.status != expected_status:
        raise RuntimeError(
            f"Stream ACK status mismatch: expected {expected_status}, got {ack.status} ({ack.status_name})"
        )

    return {
        "ack": asdict(ack),
        "ack_status_name": ack.status_name,
        "wire_stats": stats,
    }


def wait_awg_stream_done(
    uart: "UartCoordinator",
    args: argparse.Namespace,
    *,
    timeout_s: float,
) -> Any:
    deadline = time.monotonic() + timeout_s
    last_status = None
    while time.monotonic() < deadline:
        last_status = query_awg_stream_status(uart, args)
        if last_status.error:
            raise RuntimeError(f"Stream scheduler entered error state: {asdict(last_status)}")
        if last_status.done and last_status.eof_seen:
            return last_status
        time.sleep(args.scheduler_stream_status_poll_s)

    raise TimeoutError(f"Timed out waiting for stream done/EOF; last_status={last_status}")


def build_scheduler_transport_manifest(args: argparse.Namespace) -> dict:
    return {
        "selected_transport": args.scheduler_transport,
        "preload_limit_events": 256,
        "uartlite_ascii_hex_stream": {
            "raw_baud": AWG_STREAM_UART_RAW_BAUD,
            "raw_bytes_per_s": AWG_STREAM_UART_RAW_BYTES_PER_S,
            "event_size_bytes": AWG_EVENT_V1_SIZE,
            "wire_bytes_per_event_ascii_hex": AWG_STREAM_UART_HEX_BYTES_PER_EVENT,
            "expected_sustained_events_per_s": list(AWG_STREAM_UART_EXPECTED_EVENTS_PER_S),
            "dense_10k_event_expected_s": {
                "at_100_events_per_s": estimate_uartlite_stream_seconds(10_000, 100.0),
                "at_150_events_per_s": estimate_uartlite_stream_seconds(10_000, 150.0),
            },
            "soak_100k_event_expected_s": {
                "at_100_events_per_s": estimate_uartlite_stream_seconds(100_000, 100.0),
                "at_150_events_per_s": estimate_uartlite_stream_seconds(100_000, 150.0),
            },
            "throughput_claims_deferred_until": ["UART16550", "Ethernet"],
        },
        "stream_depth_sentinel": args.scheduler_stream_depth_sentinel,
    }


def assert_stream_bringup_identity(status: Any, args: argparse.Namespace) -> None:
    if status.ip_id != 0x41574753:
        raise RuntimeError(f"Unexpected scheduler IP_ID: 0x{status.ip_id:08X}")
    if status.ip_version != 0x00010000:
        raise RuntimeError(f"Unexpected scheduler IP_VERSION: 0x{status.ip_version:08X}")
    if args.scheduler_stream_depth_sentinel is not None:
        if status.stream_depth != args.scheduler_stream_depth_sentinel:
            raise RuntimeError(
                f"STREAM_DEPTH sentinel failed: expected {args.scheduler_stream_depth_sentinel}, "
                f"observed {status.stream_depth}"
            )


def build_scheduler_benchmark_catalog() -> dict:
    return {
        "scheduler_console_transport": {
            "mode": "uart_ascii_hex",
            "commands": [
                "INFO",
                "STATUS",
                "LOADBIN <count>",
                "RUN",
                "ABORT",
                "DUMP",
                "STREAMINFO",
                "STREAMSTATUS",
                "STREAMRESET",
                "STREAMHEX <bytes>",
                "EXIT",
            ],
            "batch_reuse_supported": True,
        },
        "stream_transport": {
            "mode": "uartlite_ascii_hex_frames",
            "purpose": "correctness_and_observability_not_throughput",
            "raw_baud": AWG_STREAM_UART_RAW_BAUD,
            "raw_bytes_per_s": AWG_STREAM_UART_RAW_BYTES_PER_S,
            "event_size_bytes": AWG_EVENT_V1_SIZE,
            "wire_bytes_per_event_ascii_hex": AWG_STREAM_UART_HEX_BYTES_PER_EVENT,
            "expected_sustained_events_per_s": list(AWG_STREAM_UART_EXPECTED_EVENTS_PER_S),
            "dense_10k_event_expected_s": {
                "at_100_events_per_s": estimate_uartlite_stream_seconds(10_000, 100.0),
                "at_150_events_per_s": estimate_uartlite_stream_seconds(10_000, 150.0),
            },
            "soak_100k_event_expected_s": {
                "at_100_events_per_s": estimate_uartlite_stream_seconds(100_000, 100.0),
                "at_150_events_per_s": estimate_uartlite_stream_seconds(100_000, 150.0),
            },
        },
        "known_hardware_limits": {
            "event_ram_is_finite": True,
            "legacy_preload_comparison_limit_events": 256,
            "current_firmware_reports_max_events_at_runtime": True,
            "dense_sweeps_may_require_batching": True,
            "stream_depth_sentinel_events": 511,
        },
        "stream_bringup_capabilities": [
            {
                "id": "stream_identity_depth_sentinel",
                "kind": "implemented",
                "measures": ["IP_ID", "IP_VERSION", "observed STREAM_DEPTH == 511"],
            },
            {
                "id": "stream_finite_eof",
                "kind": "implemented",
                "measures": ["ACK status", "EOF seen", "done/error bits", "STREAM_PUSHES"],
            },
            {
                "id": "stream_bad_crc",
                "kind": "implemented",
                "measures": ["BAD_CRC ACK", "unchanged accepted counters"],
            },
            {
                "id": "stream_reset_recovery",
                "kind": "implemented",
                "measures": ["soft-reset status", "FIFO occupancy/free", "stream counters"],
            },
        ],
        "fsh_capabilities": [
            {
                "id": "scheduler_dense_sweep",
                "kind": "implemented",
                "measures": [
                    "carrier frequency tracking",
                    "power flatness",
                    "step-by-step deterministic execution",
                    "batch-over-batch continuity summary",
                ],
            },
            {
                "id": "scheduler_sfdr_spot_set",
                "kind": "implemented",
                "measures": [
                    "steady-state carrier power",
                    "strongest spur within configured search band",
                    "SFDR at scheduler-held tones",
                ],
            },
            {
                "id": "scheduler_phase_noise_offsets",
                "kind": "planned",
                "measures": [
                    "marker-only sideband offsets during scheduler-held tones",
                ],
            },
        ],
        "mso22_capabilities": [
            {
                "id": "epoch_to_first_event_latency",
                "kind": "planned",
                "measures": ["time from scheduler epoch reload to first observable output edge"],
            },
            {
                "id": "event_to_event_switch_latency",
                "kind": "planned",
                "measures": ["hop latency between adjacent programmed scheduler events"],
            },
            {
                "id": "minimum_stable_dwell",
                "kind": "planned",
                "measures": ["minimum dwell/pulse width that still yields stable output"],
            },
            {
                "id": "pulse_width_and_spacing_accuracy",
                "kind": "planned",
                "measures": ["actual high/low width and inter-pulse spacing versus programmed ticks"],
            },
            {
                "id": "batch_boundary_gap",
                "kind": "planned",
                "measures": ["dead time inserted by host-side batching across 64-event boundaries"],
            },
            {
                "id": "run_to_observable_output_latency",
                "kind": "planned",
                "measures": ["time from RUN command acceptance to first observable analog/digital effect"],
            },
        ],
    }


def build_scheduler_scope_plan(args: argparse.Namespace) -> dict:
    return {
        "instrument": "Tektronix MSO22",
        "execution_status": "planned_not_executed_by_host",
        "why": (
            "The scheduler-native suite now emits a concrete MSO22 benchmark plan, but this repo "
            "does not yet contain a validated MSO22 SCPI driver path for the FMCDAC bench."
        ),
        "recommended_channel_map": {
            "CH1": "RF envelope detector, mixer IF, or representative analog output timing observable",
            "CH2": "marker_commit routed from awg_timed_ctrl; marker_start/marker_done are secondary checks",
            "optional_refill_observable": "IRQ_LOW_WATERMARK or another stream refill-margin marker if routed",
        },
        "gating_prerequisite": {
            "status": "must_verify_before_timing_measurements",
            "required_hdl_outputs": ["marker_commit", "marker_start", "marker_done"],
            "preferred_scope_cross_check": "marker_commit edge count versus STREAM_PUSHES and commit/fire counters",
        },
        "benchmarks": [
            {
                "name": "epoch_to_first_event_latency",
                "goal": "measure delay from scheduler epoch anchor to first observable output change",
                "requires": ["repeatable trigger source", "marker_start or marker_commit", "observable event edge"],
            },
            {
                "name": "event_to_event_switch_latency",
                "goal": "measure retune latency between adjacent scheduled carrier events",
                "requires": ["two-tone or frequency-hop sequence", "marker_commit", "observable edge metric"],
            },
            {
                "name": "minimum_stable_dwell_sweep",
                "goal": "target MIN_SPACING_TICKS=8 at sched_clk=245.76 MHz and verify IRQ_SPACING_VIOLATION below it",
                "requires": ["scheduler batch runner", "stable trigger path", "IRQ/status readback"],
            },
            {
                "name": "pulse_width_and_spacing_accuracy",
                "goal": "compare actual pulse widths/spacing to programmed scheduler tick intervals",
                "requires": ["observable pulse waveform or marker_commit"],
            },
            {
                "name": "batch_boundary_gap",
                "goal": "measure preload boundary overhead and compare against stream mode for schedules that fit both paths",
                "requires": ["dense multi-batch schedule", "marker around batch transitions"],
            },
            {
                "name": "stream_refill_margin_visualization",
                "goal": "visualize LOW_WATERMARK/refill behavior while cross-checking register counters",
                "requires": ["marker_commit", "IRQ_LOW_WATERMARK or equivalent refill observable"],
            },
            {
                "name": "reinit_vs_no_reinit_visibility",
                "goal": "compare first-event phase-reinitialized behavior to continuous phase steps",
                "requires": ["phase-sensitive observable or IQ-derived timing metric"],
            },
        ],
        "requested_run_args": {
            "awg_sweep_start_hz": args.awg_sweep_start_hz,
            "awg_sweep_stop_hz": args.awg_sweep_stop_hz,
            "awg_sweep_step_hz": args.awg_sweep_step_hz,
            "awg_sweep_dwell_us": args.awg_sweep_dwell_us,
        },
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Drive FMCDAC UART diagnostics and capture R&S FSH8 analyzer traces."
    )
    parser.add_argument("--serial-port", help="UART COM port, for example COM4")
    parser.add_argument("--baudrate", type=int, default=115200, help="UART baud rate")
    parser.add_argument(
        "--uart-timeout",
        type=float,
        default=120.0,
        help="Seconds to wait for each UART prompt",
    )
    parser.add_argument(
        "--clock-choice",
        default="",
        help="Legacy clock menu response. Default sends ENTER for firmware default.",
    )
    parser.add_argument(
        "--rate-choice",
        default="",
        help="Legacy rate menu response. Default sends ENTER for firmware default.",
    )
    parser.add_argument(
        "--resume-at-nco",
        action="store_true",
        help="Skip the older firmware boot menus and wait directly for the NCO prompt.",
    )
    parser.add_argument(
        "--skip-make-run",
        action="store_true",
        help="Assume the board is already running and do not launch the XSDB reset/download/start sequence first.",
    )
    parser.add_argument(
        "--make-args",
        default="",
        help="Legacy no-op kept for CLI compatibility.",
    )
    parser.add_argument(
        "--xilinx-settings",
        action="append",
        default=[],
        help="Optional settings64.bat path(s) to call before launching XSDB. Repeat as needed.",
    )
    parser.add_argument(
        "--xsdb-exe",
        default="xsdb",
        help="XSDB executable used to reset/download/start the already-built ELF.",
    )
    parser.add_argument(
        "--xsdb-hw-url",
        default="tcp:127.0.0.1:3121",
        help="hw_server URL for XSDB connect -url.",
    )
    parser.add_argument(
        "--xsdb-target-filter",
        default='name =~ "*microblaze*#0" && bscan=="USER2"',
        help="XSDB targets -set -filter expression for the processor target.",
    )
    parser.add_argument(
        "--xsdb-elf",
        default="",
        help="Optional ELF to download with XSDB. Defaults to projects/fmcdac/build/fmcdac.elf.",
    )
    parser.add_argument(
        "--xsdb-xsa",
        default="",
        help="Optional XSA to load with XSDB. Defaults to projects/fmcdac/build/tmp/system_top.xsa.",
    )
    parser.add_argument(
        "--xsdb-reset-delay-ms",
        type=int,
        default=3000,
        help="Delay after XSDB system reset before dow/con.",
    )
    parser.add_argument(
        "--xsdb-skip-loadhw",
        action="store_true",
        help="Skip XSDB loadhw -hw <xsa> -regs before reset/download/start.",
    )
    parser.add_argument(
        "--visa-resource",
        help="PyVISA resource string for the FSH8, for example TCPIP::192.168.100.135::INSTR",
    )
    parser.add_argument(
        "--visa-backend",
        default=None,
        help="Optional PyVISA backend, for example @py",
    )
    parser.add_argument(
        "--list-visa",
        action="store_true",
        help="List VISA resources and exit",
    )
    parser.add_argument(
        "--analyzer-timeout",
        type=float,
        default=30.0,
        help="Analyzer VISA timeout in seconds",
    )
    parser.add_argument(
        "--analyzer-preset",
        choices=["off", "system", "reset"],
        default="off",
        help="Apply an analyzer-wide preset/reset after connect. Use for state-control debugging on FSH8.",
    )
    parser.add_argument(
        "--dump-analyzer-state",
        action="store_true",
        help="Attach analyzer readback state and error queue snapshots to per-step JSON artifacts.",
    )
    parser.add_argument(
        "--write-step-csv",
        action="store_true",
        help="Write per-step CSV artifacts in addition to the run-level aggregate CSV outputs.",
    )
    parser.add_argument(
        "--freq-settle-timeout-s",
        type=float,
        default=DEFAULT_FREQ_SETTLE_TIMEOUT_S,
        help="For DDS-band and SFDR steps, keep re-measuring until the frequency error is within tolerance or this timeout expires. Use 0 to disable.",
    )
    parser.add_argument(
        "--freq-settle-error-hz",
        type=float,
        default=DEFAULT_FREQ_SETTLE_ERROR_HZ,
        help="Maximum acceptable absolute frequency error before a DDS-band or SFDR measurement is considered settled.",
    )
    parser.add_argument(
        "--make-timeout",
        type=float,
        default=300.0,
        help="Timeout for the XSDB reset/download/start sequence in seconds",
    )
    parser.add_argument(
        "--rbw-hz",
        type=float,
        default=100_000.0,
        help="FSH8 resolution bandwidth in Hz",
    )
    parser.add_argument(
        "--vbw-hz",
        type=float,
        default=100_000.0,
        help="FSH8 video bandwidth in Hz",
    )
    parser.add_argument(
        "--sweep-count",
        type=int,
        default=3,
        help="Number of sweeps per single measurement",
    )
    parser.add_argument(
        "--trace-mode",
        choices=sorted(TRACE_MODE_TOKENS.keys()),
        default="average",
        help="FSH8 trace mode",
    )
    parser.add_argument(
        "--detector",
        choices=sorted(DETECTOR_TOKENS.keys()),
        default="positive",
        help="FSH8 detector mode",
    )
    parser.add_argument(
        "--reference-level-dbm",
        type=float,
        default=0.0,
        help="FSH8 display reference level in dBm",
    )
    parser.add_argument(
        "--display-range-db",
        type=float,
        default=80.0,
        help="FSH8 vertical display range in dB",
    )
    parser.add_argument(
        "--attenuation-auto",
        choices=["on", "off"],
        default="on",
        help="Leave FSH8 attenuation coupled to the reference level",
    )
    parser.add_argument(
        "--preamplifier",
        choices=["on", "off"],
        default="off",
        help="Enable or disable the FSH8 preamplifier",
    )
    parser.add_argument(
        "--input-impedance",
        choices=["50", "75"],
        default="50",
        help="FSH8 input impedance setting",
    )
    parser.add_argument(
        "--capture-trace",
        action="store_true",
        help="Also read back TRAC:DATA? TRACE1 for DDS-band and optional NCO steps. SFDR uses segmented marker sweeps to avoid FSH8 trace-transfer timeouts.",
    )
    parser.add_argument(
        "--run-awg-sweep",
        action="store_true",
        help="Run the dedicated AWG scheduler UART console path with host-uploaded events.",
    )
    parser.add_argument(
        "--run-full-integration",
        action="store_true",
        help="Run the uploaded AWG scheduler pass first, then rerun the normal analyzer benchmark suite as a second pass.",
    )
    parser.add_argument(
        "--run-scheduler-benchmark-suite",
        action="store_true",
        help="Run the scheduler-native benchmark suite instead of the legacy prompt-driven analyzer flow.",
    )
    parser.add_argument(
        "--scheduler-suite-profile",
        choices=["dense", "sfdr", "fsh", "stream-bringup", "scope-plan", "all"],
        default="all",
        help=(
            "Choose which scheduler-native benchmark families to run. "
            "'dense' runs chunked stepped-tone FSH validation, 'sfdr' runs scheduler-held SFDR spots, "
            "'fsh' runs both FSH families, 'stream-bringup' runs UARTLite stream correctness checks, "
            "'scope-plan' only emits the MSO22 benchmark plan, and 'all' runs the FSH families plus writes the scope plan."
        ),
    )
    parser.add_argument(
        "--scheduler-transport",
        choices=["preload", "stream", "compare"],
        default="preload",
        help=(
            "Scheduler execution transport. 'preload' uses LOADBIN/RUN, 'stream' uses STREAMHEX frames, "
            "and 'compare' is reserved for schedules that fit both preload and stream."
        ),
    )
    parser.add_argument(
        "--scheduler-stream-depth-sentinel",
        type=int,
        default=511,
        help="Expected STREAM_DEPTH register value for stream bring-up. Use a negative value to disable the sentinel.",
    )
    parser.add_argument(
        "--scheduler-stream-frame-events",
        type=int,
        default=16,
        help="Maximum AWG events per UARTLite STREAMHEX frame.",
    )
    parser.add_argument(
        "--scheduler-stream-bringup-events",
        type=int,
        default=0,
        help="Depth-plus refill event count for stream bring-up. Default uses STREAM_DEPTH+16 after reading hardware.",
    )
    parser.add_argument(
        "--scheduler-stream-dwell-us",
        type=int,
        default=20_000,
        help="Dwell per stream bring-up event. Default is slow enough for 115200-baud ASCII-hex UARTLite.",
    )
    parser.add_argument(
        "--scheduler-stream-wait-timeout-s",
        type=float,
        default=120.0,
        help="Timeout while waiting for stream EOF/done in correctness profiles.",
    )
    parser.add_argument(
        "--scheduler-stream-status-poll-s",
        type=float,
        default=0.25,
        help="Polling interval for STREAMSTATUS while waiting for EOF/done.",
    )
    parser.add_argument(
        "--scheduler-stream-hex-line-chars",
        type=int,
        default=64,
        help="Hex characters sent per UART write while sending STREAMHEX payloads.",
    )
    parser.add_argument(
        "--scheduler-stream-hex-chunk-delay-s",
        type=float,
        default=0.002,
        help="Delay between STREAMHEX payload chunks to avoid overrunning UARTLite console input.",
    )
    parser.add_argument(
        "--scheduler-suite-sfdr-dwell-us",
        type=int,
        default=3_000_000,
        help="Hold time per scheduler SFDR spot in microseconds.",
    )
    parser.add_argument(
        "--awg-sweep-start-hz",
        type=float,
        default=None,
        help="AWG sweep start frequency in Hz (requires stop/step).",
    )
    parser.add_argument(
        "--awg-sweep-stop-hz",
        type=float,
        default=None,
        help="AWG sweep stop frequency in Hz (requires start/step).",
    )
    parser.add_argument(
        "--awg-sweep-step-hz",
        type=float,
        default=None,
        help="AWG sweep step frequency in Hz (requires start/stop).",
    )
    parser.add_argument(
        "--awg-sweep-dwell-us",
        type=int,
        default=None,
        help="AWG sweep dwell time per step in microseconds.",
    )
    parser.add_argument(
        "--awg-sweep-scale-u",
        type=int,
        default=None,
        help="AWG sweep DDS scale in micro-units (1.0=1_000_000).",
    )
    parser.add_argument(
        "--awg-sweep-start-ticks",
        type=int,
        default=None,
        help="AWG sweep start tick offset (scheduler ticks). Defaults to a safe host-computed startup margin.",
    )
    parser.add_argument(
        "--awg-sweep-tone",
        type=int,
        default=None,
        help="AWG sweep tone index (usually 0).",
    )
    parser.add_argument(
        "--awg-sched-baseaddr",
        type=lambda text: int(text, 0),
        default=None,
        help="AWG scheduler base address (e.g. 0x44AA0000).",
    )
    parser.add_argument(
        "--awg-sched-max-events",
        type=int,
        default=None,
        help="Override AWG scheduler event depth in firmware.",
    )
    parser.add_argument(
        "--awg-sched-tick-hz",
        type=int,
        default=None,
        help="Override AWG scheduler tick rate used for dwell timing.",
    )
    parser.add_argument(
        "--awg-sched-timeout-ms",
        type=int,
        default=None,
        help="Override AWG scheduler done timeout in ms.",
    )
    parser.add_argument(
        "--dds-band-sweep-start-hz",
        type=float,
        default=None,
        help="Override the firmware DDS-band sweep start frequency in Hz. Requires stop and step too.",
    )
    parser.add_argument(
        "--dds-band-sweep-stop-hz",
        type=float,
        default=None,
        help="Override the firmware DDS-band sweep stop frequency in Hz. Requires start and step too.",
    )
    parser.add_argument(
        "--dds-band-sweep-step-hz",
        type=float,
        default=None,
        help="Override the firmware DDS-band sweep step in Hz. Requires start and stop too.",
    )
    parser.add_argument(
        "--sfdr-sweep-start-hz",
        type=float,
        default=None,
        help="Override the firmware steady-state SFDR sweep start frequency in Hz. Requires stop and step too.",
    )
    parser.add_argument(
        "--sfdr-sweep-stop-hz",
        type=float,
        default=None,
        help="Override the firmware steady-state SFDR sweep stop frequency in Hz. Requires start and step too.",
    )
    parser.add_argument(
        "--sfdr-sweep-step-hz",
        type=float,
        default=None,
        help="Override the firmware steady-state SFDR sweep step in Hz. Requires start and stop too.",
    )
    parser.add_argument(
        "--sfdr-start-hz",
        type=float,
        default=DEFAULT_SFDR_MIN_SEARCH_HZ,
        help="Low end of the SFDR spur search span in Hz",
    )
    parser.add_argument(
        "--sfdr-stop-hz",
        type=float,
        default=DEFAULT_SFDR_MAX_SEARCH_HZ,
        help="High end of the SFDR spur search span in Hz",
    )
    parser.add_argument(
        "--sfdr-guard-hz",
        type=float,
        default=DEFAULT_SFDR_GUARD_HZ,
        help="Exclude this many Hz around the carrier when searching for the strongest spur",
    )
    parser.add_argument(
        "--phase-noise-span-hz",
        action="append",
        type=float,
        default=[],
        help="Capture a close-in carrier trace with this span in Hz during matching SFDR steps. Repeat the option to collect multiple spans.",
    )
    parser.add_argument(
        "--phase-noise-carrier-mhz",
        action="append",
        type=float,
        default=[],
        help="Capture close-in carrier traces for this SFDR carrier in MHz. Defaults to 400 MHz when --phase-noise-span-hz is used.",
    )
    parser.add_argument(
        "--phase-noise-rbw-hz",
        type=float,
        default=None,
        help="Override RBW for close-in carrier traces. Defaults to --rbw-hz.",
    )
    parser.add_argument(
        "--phase-noise-vbw-hz",
        type=float,
        default=None,
        help="Override VBW for close-in carrier traces. Defaults to --vbw-hz.",
    )
    parser.add_argument(
        "--phase-noise-sweep-count",
        type=int,
        default=None,
        help="Override sweep count for close-in carrier traces. Defaults to --sweep-count.",
    )
    parser.add_argument(
        "--phase-noise-trace-mode",
        choices=sorted(TRACE_MODE_TOKENS.keys()),
        default=None,
        help="Override trace mode for close-in carrier traces. Defaults to --trace-mode.",
    )
    parser.add_argument(
        "--phase-noise-detector",
        choices=sorted(DETECTOR_TOKENS.keys()),
        default=None,
        help="Override detector for close-in carrier traces. Defaults to --detector.",
    )
    parser.add_argument(
        "--phase-noise-reference-level-dbm",
        type=float,
        default=None,
        help="Override display reference level for close-in carrier traces. Defaults to --reference-level-dbm.",
    )
    parser.add_argument(
        "--phase-noise-display-range-db",
        type=float,
        default=None,
        help="Override display range for close-in carrier traces. Defaults to --display-range-db.",
    )
    parser.add_argument(
        "--phase-noise-offset-hz",
        action="append",
        type=float,
        default=[],
        help="Measure marker-only sideband levels at these carrier offsets in Hz. Repeat the option to collect multiple offsets.",
    )
    parser.add_argument(
        "--phase-noise-window-hz",
        type=float,
        default=None,
        help="Span in Hz for each marker-only phase-noise sideband window. Defaults to max(10*RBW, 1 kHz).",
    )
    parser.add_argument(
        "--dynamic-rbw-hz",
        type=float,
        default=None,
        help="Override RBW for dynamic retune capture. Defaults to --rbw-hz.",
    )
    parser.add_argument(
        "--dynamic-vbw-hz",
        type=float,
        default=None,
        help="Override VBW for dynamic retune capture. Defaults to --vbw-hz.",
    )
    parser.add_argument(
        "--dynamic-sweep-count",
        type=int,
        default=None,
        help="Override sweep count for dynamic retune capture. Defaults to 1.",
    )
    parser.add_argument(
        "--dynamic-trace-mode",
        choices=sorted(TRACE_MODE_TOKENS.keys()),
        default=None,
        help="Override trace mode for dynamic retune capture. Defaults to maxhold.",
    )
    parser.add_argument(
        "--dynamic-detector",
        choices=sorted(DETECTOR_TOKENS.keys()),
        default=None,
        help="Override detector for dynamic retune capture. Defaults to --detector.",
    )
    parser.add_argument(
        "--dynamic-reference-level-dbm",
        type=float,
        default=None,
        help="Override display reference level for dynamic retune capture. Defaults to --reference-level-dbm.",
    )
    parser.add_argument(
        "--dynamic-display-range-db",
        type=float,
        default=None,
        help="Override display range for dynamic retune capture. Defaults to --display-range-db.",
    )
    parser.add_argument(
        "--dynamic-intended-margin-hz",
        type=float,
        default=DEFAULT_DYNAMIC_INTENDED_MARGIN_HZ,
        help="Half-width search margin around each intended dynamic retune frequency.",
    )
    parser.add_argument(
        "--dynamic-start-mhz",
        action="append",
        type=float,
        default=[],
        help="Dynamic retune start carrier in MHz. Repeat with matching --dynamic-stop-mhz values.",
    )
    parser.add_argument(
        "--dynamic-stop-mhz",
        action="append",
        type=float,
        default=[],
        help="Dynamic retune stop carrier in MHz. Repeat with matching --dynamic-start-mhz values.",
    )
    parser.add_argument(
        "--dynamic-dwell-ms",
        action="append",
        type=int,
        default=[],
        help="Dynamic retune dwell in ms for each programmed tone. Repeat to request multiple dwell cases.",
    )
    parser.add_argument(
        "--dynamic-active-ms",
        type=int,
        default=12_000,
        help="Approximate total active burst duration used to derive dynamic retune transition counts.",
    )
    parser.add_argument(
        "--uart-rtt-samples",
        type=int,
        default=16,
        help="Number of UART ping/pong exchanges to use for the RTT baseline",
    )
    parser.add_argument(
        "--run-nco-test",
        action="store_true",
        help="Opt in to the firmware NCO discriminator prompt. Disabled by default because DDS-only benchmarking is the primary workflow.",
    )
    parser.add_argument(
        "--skip-nco-test",
        action="store_true",
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "--skip-dds-band-test",
        action="store_true",
        help="Skip the paused DDS-band diagnostic even if it is present",
    )
    parser.add_argument(
        "--skip-sfdr-test",
        action="store_true",
        help="Skip the paused steady-state SFDR tone set even if it is present",
    )
    parser.add_argument(
        "--skip-dynamic-sfdr-test",
        action="store_true",
        help="Skip the paused dynamic retune settling test even if it is present",
    )
    parser.add_argument(
        "--skip-throughput-test",
        action="store_true",
        help="Skip the firmware-side throughput benchmark even if it is present",
    )
    parser.add_argument(
        "--skip-uart-rtt",
        action="store_true",
        help="Skip the UART round-trip benchmark even if it is present",
    )
    parser.add_argument(
        "--output-dir",
        default=None,
        help="Directory for run artifacts. Default creates capture_runs/<timestamp> beside this script.",
    )
    parser.add_argument(
        "--serial-dtr",
        choices=["on", "off", "leave"],
        default="leave",
        help="Control UART DTR when opening the port",
    )
    parser.add_argument(
        "--serial-rts",
        choices=["on", "off", "leave"],
        default="leave",
        help="Control UART RTS when opening the port",
    )
    return parser.parse_args()


def tristate_arg(value: str) -> Optional[bool]:
    if value == "leave":
        return None
    return value == "on"


def parse_optional_sweep_range(
    start_hz: Optional[float],
    stop_hz: Optional[float],
    step_hz: Optional[float],
    label: str,
) -> Optional[SweepRange]:
    values = (start_hz, stop_hz, step_hz)
    if all(value is None for value in values):
        return None
    if any(value is None for value in values):
        raise SystemExit(f"{label} sweep override requires start, stop, and step together")
    assert start_hz is not None
    assert stop_hz is not None
    assert step_hz is not None
    if start_hz <= 0 or stop_hz <= 0 or step_hz <= 0:
        raise SystemExit(f"{label} sweep start, stop, and step must all be greater than 0")
    if stop_hz < start_hz:
        raise SystemExit(f"{label} sweep stop must be greater than or equal to start")
    span_hz = stop_hz - start_hz
    steps_float = span_hz / step_hz
    steps_rounded = round(steps_float)
    if not math.isclose(steps_float, steps_rounded, rel_tol=0.0, abs_tol=1e-9):
        raise SystemExit(f"{label} sweep range must land exactly on the stop frequency")
    if int(steps_rounded) + 1 < 1:
        raise SystemExit(f"{label} sweep must contain at least one point")
    return SweepRange(start_hz=start_hz, stop_hz=stop_hz, step_hz=step_hz)


def ensure_args(args: argparse.Namespace) -> None:
    awg_console_mode = awg_scheduler_console_requested(args)
    scheduler_suite_mode = scheduler_benchmark_suite_requested(args)

    if args.list_visa:
        resources = RohdeSchwarzFSH.list_resources(args.visa_backend)
        if resources:
            for item in resources:
                print(item)
        else:
            print("No VISA resources found.")
        raise SystemExit(0)

    if not awg_console_mode and not scheduler_suite_mode and not args.visa_resource:
        raise SystemExit("--visa-resource is required unless --list-visa is used")
    if args.run_full_integration:
        if not args.run_awg_sweep:
            raise SystemExit("--run-full-integration requires --run-awg-sweep")
        if not args.visa_resource:
            raise SystemExit("--run-full-integration requires --visa-resource")
    if scheduler_suite_mode:
        if args.run_full_integration:
            raise SystemExit("--run-scheduler-benchmark-suite cannot be combined with --run-full-integration")
        if args.scheduler_suite_profile not in ("scope-plan", "stream-bringup") and not args.visa_resource:
            raise SystemExit("--run-scheduler-benchmark-suite requires --visa-resource unless --scheduler-suite-profile=scope-plan or stream-bringup")
        if args.scheduler_transport in ("stream", "compare") and args.scheduler_suite_profile in ("dense", "sfdr", "fsh"):
            raise SystemExit(
                "--scheduler-transport stream/compare is currently gated to --scheduler-suite-profile stream-bringup "
                "or all; FSH dense/SFDR stream execution should be enabled after stream bring-up passes."
            )
    if not args.serial_port and not (scheduler_suite_mode and args.scheduler_suite_profile == "scope-plan"):
        raise SystemExit("--serial-port is required for coordinated UART + analyzer operation")
    if args.sweep_count < 1:
        raise SystemExit("--sweep-count must be at least 1")
    parse_optional_sweep_range(
        args.dds_band_sweep_start_hz,
        args.dds_band_sweep_stop_hz,
        args.dds_band_sweep_step_hz,
        "DDS-band",
    )
    parse_optional_sweep_range(
        args.sfdr_sweep_start_hz,
        args.sfdr_sweep_stop_hz,
        args.sfdr_sweep_step_hz,
        "SFDR",
    )
    parse_optional_sweep_range(
        args.awg_sweep_start_hz,
        args.awg_sweep_stop_hz,
        args.awg_sweep_step_hz,
        "AWG-sweep",
    )
    if args.scheduler_suite_sfdr_dwell_us < 1:
        raise SystemExit("--scheduler-suite-sfdr-dwell-us must be at least 1")
    if args.scheduler_stream_depth_sentinel is not None and args.scheduler_stream_depth_sentinel < 0:
        args.scheduler_stream_depth_sentinel = None
    if args.scheduler_stream_frame_events < 1:
        raise SystemExit("--scheduler-stream-frame-events must be at least 1")
    if args.scheduler_stream_bringup_events < 0:
        raise SystemExit("--scheduler-stream-bringup-events must be non-negative")
    if args.scheduler_stream_dwell_us < 1:
        raise SystemExit("--scheduler-stream-dwell-us must be at least 1")
    if args.scheduler_stream_wait_timeout_s <= 0:
        raise SystemExit("--scheduler-stream-wait-timeout-s must be greater than 0")
    if args.scheduler_stream_status_poll_s <= 0:
        raise SystemExit("--scheduler-stream-status-poll-s must be greater than 0")
    if args.scheduler_stream_hex_line_chars < 2:
        raise SystemExit("--scheduler-stream-hex-line-chars must be at least 2")
    if args.scheduler_stream_hex_chunk_delay_s < 0:
        raise SystemExit("--scheduler-stream-hex-chunk-delay-s must be >= 0")
    if args.sfdr_stop_hz <= args.sfdr_start_hz:
        raise SystemExit("--sfdr-stop-hz must be greater than --sfdr-start-hz")
    if args.sfdr_guard_hz <= 0:
        raise SystemExit("--sfdr-guard-hz must be greater than 0")
    if any(item <= 0 for item in args.phase_noise_span_hz):
        raise SystemExit("--phase-noise-span-hz values must be greater than 0")
    if any(item <= 0 for item in args.phase_noise_offset_hz):
        raise SystemExit("--phase-noise-offset-hz values must be greater than 0")
    if any(item <= 0 for item in args.phase_noise_carrier_mhz):
        raise SystemExit("--phase-noise-carrier-mhz values must be greater than 0")
    if args.phase_noise_window_hz is not None and args.phase_noise_window_hz <= 0:
        raise SystemExit("--phase-noise-window-hz must be greater than 0")
    if args.phase_noise_sweep_count is not None and args.phase_noise_sweep_count < 1:
        raise SystemExit("--phase-noise-sweep-count must be at least 1")
    if args.dynamic_sweep_count is not None and args.dynamic_sweep_count < 1:
        raise SystemExit("--dynamic-sweep-count must be at least 1")
    if args.dynamic_intended_margin_hz <= 0:
        raise SystemExit("--dynamic-intended-margin-hz must be greater than 0")
    if any(item <= 0 for item in args.dynamic_start_mhz):
        raise SystemExit("--dynamic-start-mhz values must be greater than 0")
    if any(item <= 0 for item in args.dynamic_stop_mhz):
        raise SystemExit("--dynamic-stop-mhz values must be greater than 0")
    if any(item <= 0 for item in args.dynamic_dwell_ms):
        raise SystemExit("--dynamic-dwell-ms values must be greater than 0")
    if args.dynamic_active_ms <= 0:
        raise SystemExit("--dynamic-active-ms must be greater than 0")
    if args.phase_noise_span_hz or args.phase_noise_offset_hz:
        if args.skip_sfdr_test:
            raise SystemExit("Phase-noise capture requires the SFDR prompt to run")
    elif any(
        value is not None and value != []
        for value in (
            args.phase_noise_rbw_hz,
            args.phase_noise_vbw_hz,
            args.phase_noise_sweep_count,
            args.phase_noise_trace_mode,
            args.phase_noise_detector,
            args.phase_noise_reference_level_dbm,
            args.phase_noise_display_range_db,
        )
    ) or args.phase_noise_carrier_mhz or args.phase_noise_window_hz is not None:
        raise SystemExit(
            "Phase-noise overrides require at least one --phase-noise-span-hz or --phase-noise-offset-hz"
        )
    if args.uart_rtt_samples < 1:
        raise SystemExit("--uart-rtt-samples must be at least 1")
    if args.freq_settle_timeout_s < 0:
        raise SystemExit("--freq-settle-timeout-s must be >= 0")
    if args.freq_settle_error_hz <= 0:
        raise SystemExit("--freq-settle-error-hz must be greater than 0")
    if args.awg_sched_baseaddr is not None and args.awg_sched_baseaddr < 0:
        raise SystemExit("--awg-sched-baseaddr must be non-negative")


def build_analyzer_settings(args: argparse.Namespace) -> AnalyzerSettings:
    return AnalyzerSettings(
        rbw_hz=args.rbw_hz,
        vbw_hz=args.vbw_hz,
        sweep_count=args.sweep_count,
        trace_mode=args.trace_mode,
        detector=args.detector,
        reference_level_dbm=args.reference_level_dbm,
        display_range_db=args.display_range_db,
        attenuation_auto=(args.attenuation_auto == "on"),
        preamp_on=(args.preamplifier == "on"),
        impedance_ohms=int(args.input_impedance),
        capture_trace=args.capture_trace,
    )


def build_sfdr_settings(args: argparse.Namespace) -> SfdrSettings:
    return SfdrSettings(
        search_start_hz=args.sfdr_start_hz,
        search_stop_hz=args.sfdr_stop_hz,
        carrier_guard_hz=args.sfdr_guard_hz,
    )


def build_dds_band_step_specs(args: argparse.Namespace) -> List[StepSpec]:
    custom = parse_optional_sweep_range(
        args.dds_band_sweep_start_hz,
        args.dds_band_sweep_stop_hz,
        args.dds_band_sweep_step_hz,
        "DDS-band",
    )
    freqs_hz = (
        build_uniform_freq_list(custom.start_hz, custom.stop_hz, custom.step_hz)
        if custom
        else LEGACY_DDS_BAND_FREQS_HZ
    )
    return build_single_tone_step_specs(
        group="dds_band",
        tag="DDS-BAND",
        freqs_hz=freqs_hz,
        description_prefix="DDS-band tone",
    )


def build_sfdr_step_specs(args: argparse.Namespace) -> List[StepSpec]:
    custom = parse_optional_sweep_range(
        args.sfdr_sweep_start_hz,
        args.sfdr_sweep_stop_hz,
        args.sfdr_sweep_step_hz,
        "SFDR",
    )
    freqs_hz = (
        build_uniform_freq_list(custom.start_hz, custom.stop_hz, custom.step_hz)
        if custom
        else LEGACY_SFDR_FREQS_HZ
    )
    return build_single_tone_step_specs(
        group="sfdr",
        tag="SFDR-TEST",
        freqs_hz=freqs_hz,
        description_prefix="Steady-state SFDR carrier",
    )


def build_sweep_override_cflags(args: argparse.Namespace) -> str:
    defines: List[str] = []
    dds_band = parse_optional_sweep_range(
        args.dds_band_sweep_start_hz,
        args.dds_band_sweep_stop_hz,
        args.dds_band_sweep_step_hz,
        "DDS-band",
    )
    sfdr = parse_optional_sweep_range(
        args.sfdr_sweep_start_hz,
        args.sfdr_sweep_stop_hz,
        args.sfdr_sweep_step_hz,
        "SFDR",
    )
    awg = parse_optional_sweep_range(
        args.awg_sweep_start_hz,
        args.awg_sweep_stop_hz,
        args.awg_sweep_step_hz,
        "AWG-sweep",
    )
    dynamic_cases = build_dynamic_case_matrix(args)
    if dds_band:
        defines.extend(
            [
                f"-DFMCDAC_DDS_BAND_SWEEP_START_HZ={int(round(dds_band.start_hz))}U",
                f"-DFMCDAC_DDS_BAND_SWEEP_STOP_HZ={int(round(dds_band.stop_hz))}U",
                f"-DFMCDAC_DDS_BAND_SWEEP_STEP_HZ={int(round(dds_band.step_hz))}U",
            ]
        )
    if sfdr:
        defines.extend(
            [
                f"-DFMCDAC_SFDR_SWEEP_START_HZ={int(round(sfdr.start_hz))}U",
                f"-DFMCDAC_SFDR_SWEEP_STOP_HZ={int(round(sfdr.stop_hz))}U",
                f"-DFMCDAC_SFDR_SWEEP_STEP_HZ={int(round(sfdr.step_hz))}U",
            ]
        )
    if awg:
        defines.extend(
            [
                f"-DFMCDAC_AWG_SWEEP_START_HZ={int(round(awg.start_hz))}U",
                f"-DFMCDAC_AWG_SWEEP_STOP_HZ={int(round(awg.stop_hz))}U",
                f"-DFMCDAC_AWG_SWEEP_STEP_HZ={int(round(awg.step_hz))}U",
                "-DFMCDAC_ENABLE_AWG_SWEEP_PROMPT=1",
            ]
        )
    if args.awg_sweep_dwell_us is not None:
        defines.append(f"-DFMCDAC_AWG_SWEEP_DWELL_US={args.awg_sweep_dwell_us}U")
    if args.awg_sweep_scale_u is not None:
        defines.append(f"-DFMCDAC_AWG_SWEEP_SCALE_U={args.awg_sweep_scale_u}")
    if args.awg_sweep_start_ticks is not None:
        defines.append(f"-DFMCDAC_AWG_SWEEP_START_TICKS={args.awg_sweep_start_ticks}U")
    if args.awg_sweep_tone is not None:
        defines.append(f"-DFMCDAC_AWG_SWEEP_TONE={args.awg_sweep_tone}U")
    if args.awg_sched_baseaddr is not None:
        defines.append(f"-DFMCDAC_AWG_SCHED_BASEADDR=0x{args.awg_sched_baseaddr:X}U")
    if args.awg_sched_max_events is not None:
        defines.append(f"-DFMCDAC_AWG_SCHED_MAX_EVENTS={args.awg_sched_max_events}U")
    if args.awg_sched_tick_hz is not None:
        defines.append(f"-DFMCDAC_AWG_SCHED_TICK_HZ={args.awg_sched_tick_hz}U")
    if args.awg_sched_timeout_ms is not None:
        defines.append(f"-DFMCDAC_AWG_SCHED_DONE_TIMEOUT_MS={args.awg_sched_timeout_ms}U")
    if args.run_awg_sweep:
        defines.append("-DFMCDAC_ENABLE_AWG_SWEEP_PROMPT=1")
    if dynamic_cases:
        defines.append(f"-DFMCDAC_DYNAMIC_CASE_COUNT={len(dynamic_cases)}U")
        for index, (start_hz, stop_hz, dwell_ms, transitions) in enumerate(dynamic_cases, start=1):
            defines.extend(
                [
                    f"-DFMCDAC_DYNAMIC_CASE{index}_START_HZ={int(round(start_hz))}U",
                    f"-DFMCDAC_DYNAMIC_CASE{index}_STOP_HZ={int(round(stop_hz))}U",
                    f"-DFMCDAC_DYNAMIC_CASE{index}_DWELL_MS={int(dwell_ms)}U",
                    f"-DFMCDAC_DYNAMIC_CASE{index}_TRANSITIONS={int(transitions)}U",
                ]
            )
    existing = os.environ.get("NEW_CFLAGS", "").strip()
    generated = " ".join(defines)
    return " ".join(item for item in (existing, generated) if item)


def awg_scheduler_console_requested(args: argparse.Namespace) -> bool:
    return bool(
        args.run_awg_sweep
        and args.awg_sweep_start_hz is not None
        and args.awg_sweep_stop_hz is not None
        and args.awg_sweep_step_hz is not None
    )


def scheduler_benchmark_suite_requested(args: argparse.Namespace) -> bool:
    return bool(args.run_scheduler_benchmark_suite)


def rewrite_self_invocation_args(
    argv: Sequence[str],
    *,
    output_dir: Path,
    keep_run_awg_sweep: bool,
) -> List[str]:
    rewritten: List[str] = []
    skip_next = False

    for arg in argv:
        if skip_next:
            skip_next = False
            continue

        if arg == "--run-full-integration":
            continue

        if arg == "--output-dir":
            skip_next = True
            continue

        if not keep_run_awg_sweep and arg == "--run-awg-sweep":
            continue

        rewritten.append(arg)

    rewritten.extend(["--output-dir", str(output_dir)])
    return rewritten


def run_full_integration_mode(args: argparse.Namespace) -> int:
    script_path = Path(__file__).resolve()
    script_dir = script_path.parent
    parent_output_dir = (
        Path(args.output_dir)
        if args.output_dir
        else script_dir / "capture_runs" / utc_timestamp()
    )
    parent_output_dir.mkdir(parents=True, exist_ok=True)

    awg_output_dir = parent_output_dir / "awg_scheduler_pass"
    suite_output_dir = parent_output_dir / "full_suite_pass"

    awg_cmd = [
        sys.executable,
        str(script_path),
        *rewrite_self_invocation_args(
            sys.argv[1:],
            output_dir=awg_output_dir,
            keep_run_awg_sweep=True,
        ),
    ]
    print("[HOST] Full integration pass 1/2: uploaded AWG scheduler sweep")
    print(f"[HOST] Output dir: {awg_output_dir}")
    awg_result = subprocess.run(awg_cmd, cwd=str(script_dir))
    if awg_result.returncode != 0:
        print(f"[HOST] AWG scheduler pass failed with exit code {awg_result.returncode}")
        return awg_result.returncode

    suite_cmd = [
        sys.executable,
        str(script_path),
        *rewrite_self_invocation_args(
            sys.argv[1:],
            output_dir=suite_output_dir,
            keep_run_awg_sweep=False,
        ),
    ]
    print("[HOST] Full integration pass 2/2: analyzer benchmark suite")
    print(f"[HOST] Output dir: {suite_output_dir}")
    suite_result = subprocess.run(suite_cmd, cwd=str(script_dir))
    if suite_result.returncode != 0:
        print(f"[HOST] Analyzer suite pass failed with exit code {suite_result.returncode}")
        return suite_result.returncode

    composite_summary = {
        "mode": "full_integration",
        "awg_scheduler_output_dir": str(awg_output_dir.resolve()),
        "analyzer_suite_output_dir": str(suite_output_dir.resolve()),
        "awg_scheduler_summary": str((awg_output_dir / "awg_scheduler_run.json").resolve()),
        "analyzer_suite_summary": str((suite_output_dir / "summary.json").resolve()),
        "awg_scheduler_exit_code": awg_result.returncode,
        "analyzer_suite_exit_code": suite_result.returncode,
    }
    summary_path = parent_output_dir / "full_integration_summary.json"
    summary_path.write_text(json.dumps(composite_summary, indent=2) + "\n", encoding="utf-8")
    print(f"[HOST] Full integration summary written to {summary_path}")
    return 0


def build_awg_scheduler_console_cflags(args: argparse.Namespace) -> str:
    defines: List[str] = [
        "-DFMCDAC_ENABLE_AWG_SCHED_CONSOLE=1",
        "-DFMCDAC_ENABLE_BENCHMARK_PROMPTS=0",
        "-DFMCDAC_ENABLE_SCHED_DET_SMOKETEST=0",
    ]
    if args.awg_sched_baseaddr is not None:
        defines.append(f"-DFMCDAC_AWG_SCHED_BASEADDR=0x{args.awg_sched_baseaddr:X}U")
    if args.awg_sched_max_events is not None:
        defines.append(f"-DFMCDAC_AWG_SCHED_MAX_EVENTS={args.awg_sched_max_events}U")
    if args.awg_sched_tick_hz is not None:
        defines.append(f"-DFMCDAC_AWG_SCHED_TICK_HZ={args.awg_sched_tick_hz}U")
    if args.awg_sched_timeout_ms is not None:
        defines.append(f"-DFMCDAC_AWG_SCHED_DONE_TIMEOUT_MS={args.awg_sched_timeout_ms}U")
    if args.scheduler_transport in ("stream", "compare") or args.scheduler_suite_profile == "stream-bringup":
        defines.append("-DFMCDAC_AWG_SCHED_STREAM=1")
    return " ".join(defines)


def execute_awg_scheduler_uploaded_sweep(
    uart: "UartCoordinator",
    console_log: "ConsoleLog",
    args: argparse.Namespace,
    output_dir: Path,
    analyzer: Optional["RohdeSchwarzFSH"] = None,
    analyzer_settings: Optional[AnalyzerSettings] = None,
    dump_analyzer_state: bool = False,
) -> dict:
    matched = uart.wait_for(
        AWG_CONSOLE_READY_MARKER,
        args.uart_timeout,
        extra_needles=[AWG_CONSOLE_ERROR_MARKER],
    )
    if matched == AWG_CONSOLE_ERROR_MARKER:
        raise RuntimeError("Scheduler console reported an error before ready.")
    uart.send_line("INFO")
    info_line = uart.wait_for_line_containing(AWG_CONSOLE_INFO_PREFIX, args.uart_timeout)
    info = parse_info_line(info_line)

    if info.base_addr == 0:
        raise RuntimeError("Scheduler console is enabled, but FMCDAC_AWG_SCHED_BASEADDR is 0.")

    freqs_hz = build_uniform_freq_list(
        float(args.awg_sweep_start_hz),
        float(args.awg_sweep_stop_hz),
        float(args.awg_sweep_step_hz),
    )
    if len(freqs_hz) > info.max_events:
        raise RuntimeError(
            f"Requested {len(freqs_hz)} AWG scheduler events, but firmware reports max_events={info.max_events}."
        )

    dwell_us = resolve_awg_sweep_dwell_us(args, analyzer_enabled=analyzer is not None)

    events = build_awg_sweep_events(
        freqs_hz,
        tick_hz=info.tick_hz,
        dds_clock_hz=info.dds_clock_hz,
        dds_phase_dw=info.dds_phase_dw,
        tone=args.awg_sweep_tone if args.awg_sweep_tone is not None else 0,
        scale_u=args.awg_sweep_scale_u if args.awg_sweep_scale_u is not None else 700000,
        start_ticks=args.awg_sweep_start_ticks if args.awg_sweep_start_ticks is not None else 0,
        dwell_us=dwell_us,
    )
    load_summary = load_awg_scheduler_events_into_console(uart, args, events)

    awg_step_specs = build_awg_scheduler_step_specs(freqs_hz)

    uart.send_line("RUN")
    scheduler_epoch_line = None
    scheduler_epoch_monotonic_s = None
    if analyzer is not None:
        scheduler_epoch_line, scheduler_epoch_monotonic_s = uart.wait_for_line_containing_timed(
            AWG_SET_EPOCH_ARTIFACT_PREFIX,
            args.uart_timeout,
        )

    measurement_steps: List[StepCaptureSummary] = []
    measurement_windows: List[dict] = []
    if analyzer is not None:
        if analyzer_settings is None:
            raise RuntimeError("Analyzer settings were not provided for AWG sweep validation.")
        if scheduler_epoch_monotonic_s is None:
            raise RuntimeError("Missing scheduler epoch anchor for AWG sweep validation.")

        reference_power_dbm: Optional[float] = None
        reference_step_name: Optional[str] = None

        for index, (step, event) in enumerate(zip(awg_step_specs, events), start=1):
            event_start_s = scheduler_epoch_monotonic_s + (
                float(event.timestamp_ticks) / float(info.tick_hz)
            )
            if index < len(events):
                next_event_start_s = scheduler_epoch_monotonic_s + (
                    float(events[index].timestamp_ticks) / float(info.tick_hz)
                )
            else:
                next_event_start_s = event_start_s + (dwell_us / 1_000_000.0)

            dwell_s = max(next_event_start_s - event_start_s, dwell_us / 1_000_000.0)
            settle_s = min(
                AWG_SWEEP_ANALYZER_MAX_SETTLE_S,
                max(AWG_SWEEP_ANALYZER_MIN_SETTLE_S, dwell_s * AWG_SWEEP_ANALYZER_SETTLE_FRACTION),
            )
            capture_target_s = event_start_s + settle_s

            now_s = time.monotonic()
            if now_s < capture_target_s:
                time.sleep(capture_target_s - now_s)

            capture_start_s = time.monotonic()
            if index < len(events) and capture_start_s >= next_event_start_s:
                raise RuntimeError(
                    f"Missed AWG sweep measurement window for step {index}/{len(events)} at "
                    f"{step.expected_freq_hz[0] / 1e6:.6f} MHz. Increase --awg-sweep-dwell-us."
                )

            summary = capture_known_tone_step(
                analyzer=analyzer,
                output_dir=output_dir,
                step=step,
                settings=analyzer_settings,
                dump_analyzer_state=dump_analyzer_state,
                write_csv=getattr(args, "write_step_csv", False),
                write_json=False,
            )
            capture_end_s = time.monotonic()

            if index < len(events) and capture_end_s >= next_event_start_s:
                raise RuntimeError(
                    f"Analyzer capture overran AWG sweep step {index}/{len(events)} at "
                    f"{step.expected_freq_hz[0] / 1e6:.6f} MHz. Increase --awg-sweep-dwell-us."
                )

            metrics = summary.metrics
            if reference_power_dbm is None:
                reference_power_dbm = metrics.power_dbm
                reference_step_name = step.name
            metrics.reference_power_dbm = reference_power_dbm
            metrics.reference_step_name = reference_step_name
            metrics.power_delta_db = metrics.power_dbm - reference_power_dbm

            if dump_analyzer_state:
                json_path = output_dir / f"step{step.index:02d}_{step.name}.json"
                write_step_json(
                    json_path,
                    analyzer.idn,
                    step,
                    metrics,
                    extra=build_step_extra(
                        analyzer,
                        dump_analyzer_state,
                        {
                            "scheduler_event_index": index - 1,
                            "scheduler_timestamp_ticks": event.timestamp_ticks,
                            "capture_window": {
                                "event_start_s_from_epoch": event_start_s - scheduler_epoch_monotonic_s,
                                "event_end_s_from_epoch": next_event_start_s - scheduler_epoch_monotonic_s,
                                "capture_target_s_from_epoch": capture_target_s - scheduler_epoch_monotonic_s,
                                "capture_start_s_from_epoch": capture_start_s - scheduler_epoch_monotonic_s,
                                "capture_end_s_from_epoch": capture_end_s - scheduler_epoch_monotonic_s,
                            },
                        },
                    ),
                )

            measurement_steps.append(summary)
            measurement_windows.append(
                {
                    "step_index": step.index,
                    "event_index": index - 1,
                    "expected_freq_hz": step.expected_freq_hz[0],
                    "scheduler_timestamp_ticks": event.timestamp_ticks,
                    "event_start_s_from_epoch": event_start_s - scheduler_epoch_monotonic_s,
                    "event_end_s_from_epoch": next_event_start_s - scheduler_epoch_monotonic_s,
                    "capture_target_s_from_epoch": capture_target_s - scheduler_epoch_monotonic_s,
                    "capture_start_s_from_epoch": capture_start_s - scheduler_epoch_monotonic_s,
                    "capture_end_s_from_epoch": capture_end_s - scheduler_epoch_monotonic_s,
                }
            )
            print_step_summary(step, metrics)

    matched = uart.wait_for(
        AWG_CONSOLE_RUN_DONE_MARKER,
        args.uart_timeout,
        extra_needles=[AWG_CONSOLE_ERROR_MARKER],
    )
    if matched == AWG_CONSOLE_ERROR_MARKER:
        raise RuntimeError("Scheduler console reported an error during RUN.")
    uart.send_line("EXIT")
    matched = uart.wait_for(
        AWG_CONSOLE_BYE_MARKER,
        args.uart_timeout,
        extra_needles=[AWG_CONSOLE_ERROR_MARKER],
    )
    if matched == AWG_CONSOLE_ERROR_MARKER:
        raise RuntimeError("Scheduler console reported an error during EXIT.")

    artifact = parse_last_artifact_block(console_log.file_path.read_text(encoding="utf-8"))
    summary = {
        "mode": "awg_scheduler_console",
        "console_info_initial": asdict(info),
        "info": asdict(info),
        "final_status": asdict(artifact.status),
        "completed_successfully": bool(
            artifact.status.error == 0 and artifact.status.commit_count >= len(events)
        ),
        "analyzer_validated": analyzer is not None,
        "analyzer_idn": None if analyzer is None else analyzer.idn,
        "dwell_us": dwell_us,
        "scheduler_epoch_anchor_line": scheduler_epoch_line,
        "event_count": len(events),
        "event_payload_bytes": load_summary["event_payload_bytes"],
        "event_payload_hex_chars": load_summary["event_payload_hex_chars"],
        "freqs_hz": freqs_hz,
        "events": [asdict(event) for event in events],
        "steps": [asdict(step) for step in measurement_steps],
        "measurement_windows": measurement_windows,
        "artifact": asdict(artifact),
        "uart_log": str(console_log.file_path),
    }
    summary_path = output_dir / "awg_scheduler_run.json"
    summary_path.write_text(json.dumps(summary, indent=2, default=json_default) + "\n", encoding="utf-8")
    if measurement_steps:
        write_single_tone_band_artifacts(
            summary_path,
            output_dir,
            output_dir.name,
            groups=("awg_scheduler",),
            title="AWG Scheduler Sweep Level Delta vs Frequency",
            stem="awg_sweep_plot",
        )
    print(f"[HOST] AWG scheduler run summary written to {summary_path}")
    return summary


def execute_scheduler_dense_fsh_suite(
    uart: "UartCoordinator",
    console_log: "ConsoleLog",
    args: argparse.Namespace,
    output_dir: Path,
    info: AwgSchedInfo,
    analyzer: "RohdeSchwarzFSH",
    analyzer_settings: AnalyzerSettings,
) -> dict:
    output_dir.mkdir(parents=True, exist_ok=True)
    sweep = parse_optional_sweep_range(
        args.awg_sweep_start_hz,
        args.awg_sweep_stop_hz,
        args.awg_sweep_step_hz,
        "Scheduler benchmark dense sweep",
    )
    if sweep is None:
        raise RuntimeError(
            "Scheduler benchmark dense sweep requires --awg-sweep-start-hz, "
            "--awg-sweep-stop-hz, and --awg-sweep-step-hz."
        )

    all_freqs_hz = build_uniform_freq_list(sweep.start_hz, sweep.stop_hz, sweep.step_hz)
    dwell_us = resolve_awg_sweep_dwell_us(args, analyzer_enabled=True)
    batch_specs = build_scheduler_batch_specs(all_freqs_hz, info.max_events)
    measurement_steps: List[StepCaptureSummary] = []
    measurement_windows: List[dict] = []
    batch_summaries: List[dict] = []

    reference_power_dbm: Optional[float] = None
    reference_step_name: Optional[str] = None

    for batch in batch_specs:
        batch_output_dir = output_dir / f"batch_{batch.batch_index:03d}"
        batch_output_dir.mkdir(parents=True, exist_ok=True)
        batch_freqs_hz = [int(round(item)) for item in batch.freqs_hz]
        events = build_awg_sweep_events(
            batch_freqs_hz,
            tick_hz=info.tick_hz,
            dds_clock_hz=info.dds_clock_hz,
            dds_phase_dw=info.dds_phase_dw,
            tone=args.awg_sweep_tone if args.awg_sweep_tone is not None else 0,
            scale_u=args.awg_sweep_scale_u if args.awg_sweep_scale_u is not None else 700000,
            start_ticks=args.awg_sweep_start_ticks if args.awg_sweep_start_ticks is not None else 0,
            dwell_us=dwell_us,
        )
        load_summary = load_awg_scheduler_events_into_console(uart, args, events)
        step_specs = build_scheduler_dense_step_specs(
            batch.freqs_hz,
            step_index_offset=batch.start_index,
        )

        uart.send_line("RUN")
        scheduler_epoch_line, scheduler_epoch_monotonic_s = uart.wait_for_line_containing_timed(
            AWG_SET_EPOCH_ARTIFACT_PREFIX,
            args.uart_timeout,
        )

        for index, (step, event) in enumerate(zip(step_specs, events), start=1):
            event_start_s = scheduler_epoch_monotonic_s + (
                float(event.timestamp_ticks) / float(info.tick_hz)
            )
            if index < len(events):
                next_event_start_s = scheduler_epoch_monotonic_s + (
                    float(events[index].timestamp_ticks) / float(info.tick_hz)
                )
            else:
                next_event_start_s = event_start_s + (dwell_us / 1_000_000.0)

            dwell_s = max(next_event_start_s - event_start_s, dwell_us / 1_000_000.0)
            settle_s = min(
                AWG_SWEEP_ANALYZER_MAX_SETTLE_S,
                max(AWG_SWEEP_ANALYZER_MIN_SETTLE_S, dwell_s * AWG_SWEEP_ANALYZER_SETTLE_FRACTION),
            )
            capture_target_s = event_start_s + settle_s

            now_s = time.monotonic()
            if now_s < capture_target_s:
                time.sleep(capture_target_s - now_s)

            capture_start_s = time.monotonic()
            if index < len(events) and capture_start_s >= next_event_start_s:
                raise RuntimeError(
                    f"Missed scheduler dense measurement window for batch {batch.batch_index}/{batch.total_batches}, "
                    f"step {index}/{len(events)} at {step.expected_freq_hz[0] / 1e6:.6f} MHz. "
                    "Increase --awg-sweep-dwell-us."
                )

            summary = capture_known_tone_step(
                analyzer=analyzer,
                output_dir=batch_output_dir,
                step=step,
                settings=analyzer_settings,
                dump_analyzer_state=getattr(args, "dump_analyzer_state", False),
                write_csv=getattr(args, "write_step_csv", False),
                write_json=False,
            )
            capture_end_s = time.monotonic()
            if index < len(events) and capture_end_s >= next_event_start_s:
                raise RuntimeError(
                    f"Analyzer capture overran scheduler dense step {step.index} "
                    f"at {step.expected_freq_hz[0] / 1e6:.6f} MHz. Increase --awg-sweep-dwell-us."
                )

            metrics = summary.metrics
            if reference_power_dbm is None:
                reference_power_dbm = metrics.power_dbm
                reference_step_name = step.name
            metrics.reference_power_dbm = reference_power_dbm
            metrics.reference_step_name = reference_step_name
            metrics.power_delta_db = metrics.power_dbm - reference_power_dbm

            if getattr(args, "dump_analyzer_state", False):
                json_path = batch_output_dir / f"step{step.index:05d}_{step.name}.json"
                write_step_json(
                    json_path,
                    analyzer.idn,
                    step,
                    metrics,
                    extra=build_step_extra(
                        analyzer,
                        getattr(args, "dump_analyzer_state", False),
                        {
                            "scheduler_batch_index": batch.batch_index,
                            "scheduler_total_batches": batch.total_batches,
                            "scheduler_event_index_within_batch": index - 1,
                            "scheduler_global_event_index": batch.start_index + index - 1,
                            "scheduler_timestamp_ticks": event.timestamp_ticks,
                            "capture_window": {
                                "event_start_s_from_epoch": event_start_s - scheduler_epoch_monotonic_s,
                                "event_end_s_from_epoch": next_event_start_s - scheduler_epoch_monotonic_s,
                                "capture_target_s_from_epoch": capture_target_s - scheduler_epoch_monotonic_s,
                                "capture_start_s_from_epoch": capture_start_s - scheduler_epoch_monotonic_s,
                                "capture_end_s_from_epoch": capture_end_s - scheduler_epoch_monotonic_s,
                            },
                        },
                    ),
                )

            measurement_steps.append(summary)
            measurement_windows.append(
                {
                    "batch_index": batch.batch_index,
                    "batch_total": batch.total_batches,
                    "global_step_index": step.index,
                    "event_index_within_batch": index - 1,
                    "expected_freq_hz": step.expected_freq_hz[0],
                    "scheduler_timestamp_ticks": event.timestamp_ticks,
                    "event_start_s_from_epoch": event_start_s - scheduler_epoch_monotonic_s,
                    "event_end_s_from_epoch": next_event_start_s - scheduler_epoch_monotonic_s,
                    "capture_target_s_from_epoch": capture_target_s - scheduler_epoch_monotonic_s,
                    "capture_start_s_from_epoch": capture_start_s - scheduler_epoch_monotonic_s,
                    "capture_end_s_from_epoch": capture_end_s - scheduler_epoch_monotonic_s,
                }
            )
            print_step_summary(step, metrics)

        matched = uart.wait_for(
            AWG_CONSOLE_RUN_DONE_MARKER,
            args.uart_timeout,
            extra_needles=[AWG_CONSOLE_ERROR_MARKER],
        )
        if matched == AWG_CONSOLE_ERROR_MARKER:
            raise RuntimeError("Scheduler console reported an error during dense benchmark RUN.")

        artifact = parse_last_artifact_block(console_log.file_path.read_text(encoding="utf-8"))
        batch_summaries.append(
            {
                "batch_index": batch.batch_index,
                "total_batches": batch.total_batches,
                "start_index": batch.start_index,
                "freqs_hz": batch.freqs_hz,
                "load_summary": load_summary,
                "scheduler_epoch_anchor_line": scheduler_epoch_line,
                "artifact": asdict(artifact),
            }
        )

    dense_summary = {
        "kind": "scheduler_dense_sweep",
        "freq_count": len(all_freqs_hz),
        "batch_count": len(batch_specs),
        "max_events_per_batch": info.max_events,
        "dwell_us": dwell_us,
        "steps": [asdict(step) for step in measurement_steps],
        "measurement_windows": measurement_windows,
        "batches": batch_summaries,
    }

    dense_summary_path = output_dir / "scheduler_dense_sweep.json"
    dense_summary_path.write_text(
        json.dumps(dense_summary, indent=2, default=json_default) + "\n",
        encoding="utf-8",
    )
    if measurement_steps:
        write_single_tone_band_artifacts(
            dense_summary_path,
            output_dir,
            output_dir.name,
            groups=("scheduler_dense",),
            title="Scheduler Dense Sweep Level Delta vs Frequency",
            stem="scheduler_dense_sweep_plot",
        )
    return dense_summary


def execute_scheduler_sfdr_spot_suite(
    uart: "UartCoordinator",
    console_log: "ConsoleLog",
    args: argparse.Namespace,
    output_dir: Path,
    info: AwgSchedInfo,
    analyzer: "RohdeSchwarzFSH",
    analyzer_settings: AnalyzerSettings,
    sfdr_settings: SfdrSettings,
) -> dict:
    output_dir.mkdir(parents=True, exist_ok=True)
    custom = parse_optional_sweep_range(
        args.sfdr_sweep_start_hz,
        args.sfdr_sweep_stop_hz,
        args.sfdr_sweep_step_hz,
        "Scheduler benchmark SFDR",
    )
    freqs_hz = (
        build_uniform_freq_list(custom.start_hz, custom.stop_hz, custom.step_hz)
        if custom
        else LEGACY_SFDR_FREQS_HZ
    )
    dwell_us = max(
        args.scheduler_suite_sfdr_dwell_us,
        AWG_SWEEP_ANALYZER_MIN_DWELL_US,
    )
    step_specs = build_single_tone_step_specs(
        group="scheduler_sfdr",
        tag="SCHED-SFDR",
        freqs_hz=freqs_hz,
        description_prefix="Scheduler-held SFDR carrier",
    )
    summaries: List[StepCaptureSummary] = []
    batch_summaries: List[dict] = []

    for step in step_specs:
        freq_hz = int(round(step.expected_freq_hz[0]))
        event = build_awg_sweep_events(
            [freq_hz],
            tick_hz=info.tick_hz,
            dds_clock_hz=info.dds_clock_hz,
            dds_phase_dw=info.dds_phase_dw,
            tone=args.awg_sweep_tone if args.awg_sweep_tone is not None else 0,
            scale_u=args.awg_sweep_scale_u if args.awg_sweep_scale_u is not None else 700000,
            start_ticks=args.awg_sweep_start_ticks if args.awg_sweep_start_ticks is not None else 0,
            dwell_us=dwell_us,
        )
        load_summary = load_awg_scheduler_events_into_console(uart, args, event)
        uart.send_line("RUN")
        scheduler_epoch_line, scheduler_epoch_monotonic_s = uart.wait_for_line_containing_timed(
            AWG_SET_EPOCH_ARTIFACT_PREFIX,
            args.uart_timeout,
        )

        event_start_s = scheduler_epoch_monotonic_s + (float(event[0].timestamp_ticks) / float(info.tick_hz))
        capture_target_s = event_start_s + min(
            AWG_SWEEP_ANALYZER_MAX_SETTLE_S,
            max(AWG_SWEEP_ANALYZER_MIN_SETTLE_S, (dwell_us / 1_000_000.0) * AWG_SWEEP_ANALYZER_SETTLE_FRACTION),
        )
        now_s = time.monotonic()
        if now_s < capture_target_s:
            time.sleep(capture_target_s - now_s)

        metrics = analyzer.capture_sfdr(step, analyzer_settings, sfdr_settings)
        csv_path = output_dir / f"step{step.index:03d}_{step.name}.csv"
        if getattr(args, "write_step_csv", False):
            save_trace_csv(
                csv_path,
                [metrics.power_freq_hz, metrics.spur_freq_hz or metrics.power_freq_hz],
                [metrics.power_dbm, metrics.spur_power_dbm or metrics.power_dbm],
            )
        if getattr(args, "dump_analyzer_state", False):
            json_path = output_dir / f"step{step.index:03d}_{step.name}.json"
            write_step_json(
                json_path,
                analyzer.idn,
                step,
                metrics,
                extra=build_step_extra(
                    analyzer,
                    getattr(args, "dump_analyzer_state", False),
                    {
                        "scheduler_epoch_anchor_line": scheduler_epoch_line,
                        "scheduler_timestamp_ticks": event[0].timestamp_ticks,
                        "scheduler_dwell_us": dwell_us,
                    },
                ),
            )
        summary = StepCaptureSummary(
            group=step.group,
            step_index=step.index,
            name=step.name,
            marker=step.marker,
            description=step.description,
            expected_freq_hz=step.expected_freq_hz,
            csv_path=str(csv_path.resolve()) if getattr(args, "write_step_csv", False) else "",
            metrics=metrics,
        )
        summaries.append(summary)
        print_step_summary(step, metrics)

        matched = uart.wait_for(
            AWG_CONSOLE_RUN_DONE_MARKER,
            args.uart_timeout,
            extra_needles=[AWG_CONSOLE_ERROR_MARKER],
        )
        if matched == AWG_CONSOLE_ERROR_MARKER:
            raise RuntimeError("Scheduler console reported an error during scheduler SFDR RUN.")
        artifact = parse_last_artifact_block(console_log.file_path.read_text(encoding="utf-8"))
        batch_summaries.append(
            {
                "step_index": step.index,
                "freq_hz": step.expected_freq_hz[0],
                "load_summary": load_summary,
                "artifact": asdict(artifact),
            }
        )

    summary = {
        "kind": "scheduler_sfdr_spot_set",
        "dwell_us": dwell_us,
        "steps": [asdict(step) for step in summaries],
        "batches": batch_summaries,
    }
    summary_path = output_dir / "scheduler_sfdr_spot_set.json"
    summary_path.write_text(json.dumps(summary, indent=2, default=json_default) + "\n", encoding="utf-8")
    write_sfdr_results_csv(summaries, output_dir / "scheduler_sfdr_spot_set.csv")
    return summary


def execute_scheduler_stream_bringup_suite(
    uart: "UartCoordinator",
    console_log: "ConsoleLog",
    args: argparse.Namespace,
    output_dir: Path,
    info: Any,
) -> dict:
    output_dir.mkdir(parents=True, exist_ok=True)

    dds_clock_hz = info.dds_clock_hz or 983_056_640
    dds_phase_dw = info.dds_phase_dw or 32
    seq = 1

    initial_status = query_awg_stream_status(uart, args, command="STREAMINFO")
    assert_stream_bringup_identity(initial_status, args)
    after_reset = reset_awg_stream_console(uart, args)
    assert_stream_bringup_identity(after_reset, args)

    before_bad_crc = query_awg_stream_status(uart, args)
    bad_crc_frame = pack_stream_frame([], seq=seq, corrupt_crc=True)
    bad_crc_result = send_awg_stream_frame_hex(
        uart,
        args,
        bad_crc_frame,
        expected_status=4,
    )
    seq += 1
    after_bad_crc = query_awg_stream_status(uart, args)
    if after_bad_crc.stream_pushes != before_bad_crc.stream_pushes:
        raise RuntimeError(
            "Bad CRC changed STREAM_PUSHES: "
            f"before={before_bad_crc.stream_pushes} after={after_bad_crc.stream_pushes}"
        )

    after_bad_crc_reset = reset_awg_stream_console(uart, args)
    finite_events = build_awg_sweep_events(
        [200_000_000],
        tick_hz=info.tick_hz,
        dds_clock_hz=dds_clock_hz,
        dds_phase_dw=dds_phase_dw,
        tone=0,
        scale_u=args.awg_sweep_scale_u or 700_000,
        start_ticks=args.awg_sweep_start_ticks or 10_000,
        dwell_us=args.scheduler_stream_dwell_us,
    )
    finite_frame = pack_stream_frame(
        finite_events,
        seq=seq,
        open_stream=True,
        close_with_eof=True,
    )
    finite_result = send_awg_stream_frame_hex(uart, args, finite_frame)
    seq += 1
    finite_done = wait_awg_stream_done(
        uart,
        args,
        timeout_s=args.scheduler_stream_wait_timeout_s,
    )

    after_finite_reset = reset_awg_stream_console(uart, args)
    refill_event_count = args.scheduler_stream_bringup_events
    if refill_event_count <= 0:
        refill_event_count = after_finite_reset.stream_depth + 16
    refill_freqs = [200_000_000 + (idx * 100_000) for idx in range(refill_event_count)]
    refill_events = build_awg_sweep_events(
        refill_freqs,
        tick_hz=info.tick_hz,
        dds_clock_hz=dds_clock_hz,
        dds_phase_dw=dds_phase_dw,
        tone=0,
        scale_u=args.awg_sweep_scale_u or 700_000,
        start_ticks=args.awg_sweep_start_ticks or 10_000,
        dwell_us=args.scheduler_stream_dwell_us,
    )
    frame_events = max(1, args.scheduler_stream_frame_events)
    refill_results = []
    for frame_index, offset in enumerate(range(0, len(refill_events), frame_events)):
        chunk = refill_events[offset : offset + frame_events]
        refill_frame = pack_stream_frame(
            chunk,
            seq=seq,
            open_stream=(frame_index == 0),
            close_with_eof=(offset + frame_events >= len(refill_events)),
        )
        refill_results.append(send_awg_stream_frame_hex(uart, args, refill_frame))
        seq += 1
    refill_done = wait_awg_stream_done(
        uart,
        args,
        timeout_s=max(
            args.scheduler_stream_wait_timeout_s,
            (refill_event_count * args.scheduler_stream_dwell_us / 1_000_000.0) + 10.0,
        ),
    )
    if refill_done.stream_pushes < refill_done.commit_count:
        raise RuntimeError(
            "Stream counter drift: STREAM_PUSHES is below commit count "
            f"({refill_done.stream_pushes} < {refill_done.commit_count})"
        )

    final_status = query_awg_stream_status(uart, args)
    stream_counter_check = {
        "stream_pushes_minus_commit": refill_done.stream_pushes - refill_done.commit_count,
        "final_free_space": refill_done.free_space,
        "final_occupancy": refill_done.occupancy,
        "free_space_plus_occupancy": refill_done.free_space + refill_done.occupancy,
        "stream_depth": refill_done.stream_depth,
        "free_space_occupancy_matches_depth": (
            refill_done.free_space + refill_done.occupancy == refill_done.stream_depth
        ),
    }
    summary = {
        "mode": "scheduler_stream_bringup",
        "transport_manifest": build_scheduler_transport_manifest(args),
        "console_info_initial": asdict(info),
        "dds_clock_hz_used": dds_clock_hz,
        "dds_phase_dw_used": dds_phase_dw,
        "initial_status": asdict(initial_status),
        "after_reset": asdict(after_reset),
        "bad_crc": {
            "before": asdict(before_bad_crc),
            "result": bad_crc_result,
            "after": asdict(after_bad_crc),
            "counter_unchanged": after_bad_crc.stream_pushes == before_bad_crc.stream_pushes,
        },
        "after_bad_crc_reset": asdict(after_bad_crc_reset),
        "finite_eof": {
            "events_requested": len(finite_events),
            "result": finite_result,
            "done_status": asdict(finite_done),
        },
        "after_finite_reset": asdict(after_finite_reset),
        "depth_plus_refill": {
            "events_requested": len(refill_events),
            "frame_events": frame_events,
            "frames": refill_results,
            "done_status": asdict(refill_done),
            "counter_check": stream_counter_check,
        },
        "final_status": asdict(final_status),
        "uart_log": str(console_log.file_path.resolve()),
    }
    summary_path = output_dir / "scheduler_stream_bringup.json"
    summary_path.write_text(json.dumps(summary, indent=2, default=json_default) + "\n", encoding="utf-8")
    return summary


def run_scheduler_benchmark_suite_mode(args: argparse.Namespace) -> int:
    script_dir = Path(__file__).resolve().parent
    output_dir = Path(args.output_dir) if args.output_dir else script_dir / "capture_runs" / utc_timestamp()
    output_dir.mkdir(parents=True, exist_ok=True)

    catalog = build_scheduler_benchmark_catalog()
    scope_plan = build_scheduler_scope_plan(args)
    (output_dir / "scheduler_benchmark_catalog.json").write_text(
        json.dumps(catalog, indent=2, default=json_default) + "\n",
        encoding="utf-8",
    )
    (output_dir / "scheduler_scope_plan.json").write_text(
        json.dumps(scope_plan, indent=2, default=json_default) + "\n",
        encoding="utf-8",
    )

    if args.scheduler_suite_profile == "scope-plan":
        print(f"[HOST] Scheduler scope plan written to {output_dir / 'scheduler_scope_plan.json'}")
        return 0

    console_log = ConsoleLog(output_dir / "uart.log")
    uart = None
    analyzer = None
    try:
        analyzer_settings = None
        sfdr_settings = None
        if args.scheduler_suite_profile != "stream-bringup":
            analyzer = RohdeSchwarzFSH(args.visa_resource, args.visa_backend, args.analyzer_timeout)
            print(f"[HOST] Analyzer connected: {analyzer.idn}")
            if args.analyzer_preset != "off":
                print(f"[HOST] Applying analyzer preset: {args.analyzer_preset}")
                analyzer.apply_preset(args.analyzer_preset)
                print("[HOST] Analyzer preset complete.")

            analyzer_settings = build_awg_scheduler_analyzer_settings(build_analyzer_settings(args))
            sfdr_settings = build_sfdr_settings(args)
        uart = UartCoordinator(
            port=args.serial_port,
            baudrate=args.baudrate,
            timeout_s=args.uart_timeout,
            log=console_log,
            dtr=tristate_arg(args.serial_dtr),
            rts=tristate_arg(args.serial_rts),
        )
        print(f"[HOST] UART connected: {args.serial_port} @ {args.baudrate}")

        settings_files = resolve_xilinx_settings(args)
        extra_cflags = build_awg_scheduler_console_cflags(args)
        if extra_cflags:
            print(
                "[HOST] WARNING: generated firmware override CFLAGS are not applied by XSDB launch; "
                "ensure the selected ELF was built with the expected scheduler console options."
            )
        if not args.skip_make_run:
            rx_before = uart.rx_count
            for item in settings_files:
                print(f"[HOST] Using Xilinx settings: {item}")
            print("[HOST] Launching XSDB reset/download/start for scheduler benchmark suite...")
            run_xsdb_launch(
                project_dir=script_dir,
                output_dir=output_dir,
                uart=uart,
                timeout_s=args.make_timeout,
                settings_files=settings_files,
                args=args,
            )
            print("[HOST] XSDB launch completed.")
            if uart.rx_count == rx_before:
                uart.reopen()

        matched = uart.wait_for(
            AWG_CONSOLE_READY_MARKER,
            args.uart_timeout,
            extra_needles=[AWG_CONSOLE_ERROR_MARKER],
        )
        if matched == AWG_CONSOLE_ERROR_MARKER:
            raise RuntimeError("Scheduler console reported an error before ready.")
        uart.send_line("INFO")
        info_line = uart.wait_for_line_containing(AWG_CONSOLE_INFO_PREFIX, args.uart_timeout)
        info = parse_info_line(info_line)

        suite_summary = {
            "mode": "scheduler_benchmark_suite",
            "profile": args.scheduler_suite_profile,
            "transport": args.scheduler_transport,
            "transport_manifest": build_scheduler_transport_manifest(args),
            "transport_execution_note": (
                "FSH dense/SFDR profiles remain preload-based; stream transport currently gates "
                "the stream_bringup profile before RF profiles are switched."
            ),
            "catalog_path": str((output_dir / "scheduler_benchmark_catalog.json").resolve()),
            "scope_plan_path": str((output_dir / "scheduler_scope_plan.json").resolve()),
            "console_info_initial": asdict(info),
            "stream_bringup": None,
            "dense_sweep": None,
            "sfdr_spot_set": None,
        }

        if args.scheduler_suite_profile == "stream-bringup" or (
            args.scheduler_suite_profile == "all" and args.scheduler_transport in ("stream", "compare")
        ):
            suite_summary["stream_bringup"] = execute_scheduler_stream_bringup_suite(
                uart=uart,
                console_log=console_log,
                args=args,
                output_dir=output_dir / "stream_bringup",
                info=info,
            )

        if args.scheduler_suite_profile in ("dense", "fsh", "all"):
            if analyzer is None or analyzer_settings is None:
                raise RuntimeError("FSH scheduler suite requires analyzer setup")
            suite_summary["dense_sweep"] = execute_scheduler_dense_fsh_suite(
                uart=uart,
                console_log=console_log,
                args=args,
                output_dir=output_dir / "dense_sweep",
                info=info,
                analyzer=analyzer,
                analyzer_settings=analyzer_settings,
            )

        if args.scheduler_suite_profile in ("sfdr", "fsh", "all"):
            if analyzer is None or analyzer_settings is None or sfdr_settings is None:
                raise RuntimeError("SFDR scheduler suite requires analyzer setup")
            suite_summary["sfdr_spot_set"] = execute_scheduler_sfdr_spot_suite(
                uart=uart,
                console_log=console_log,
                args=args,
                output_dir=output_dir / "sfdr_spots",
                info=info,
                analyzer=analyzer,
                analyzer_settings=analyzer_settings,
                sfdr_settings=sfdr_settings,
            )

        uart.send_line("EXIT")
        uart.wait_for(AWG_CONSOLE_BYE_MARKER, args.uart_timeout, extra_needles=[AWG_CONSOLE_ERROR_MARKER])

        summary_path = output_dir / "scheduler_benchmark_suite.json"
        summary_path.write_text(
            json.dumps(suite_summary, indent=2, default=json_default) + "\n",
            encoding="utf-8",
        )
        print(f"[HOST] Scheduler benchmark suite summary written to {summary_path}")
        return 0
    except Exception as exc:
        print(f"[HOST] Scheduler benchmark suite error: {exc}")
        return 1
    finally:
        if uart is not None:
            try:
                uart.close()
            except Exception:
                pass
        if analyzer is not None:
            try:
                analyzer.close()
            except Exception:
                pass
        console_log.close()


def run_awg_scheduler_console_mode(args: argparse.Namespace) -> int:
    script_dir = Path(__file__).resolve().parent
    output_dir = Path(args.output_dir) if args.output_dir else script_dir / "capture_runs" / utc_timestamp()
    output_dir.mkdir(parents=True, exist_ok=True)

    console_log = ConsoleLog(output_dir / "uart.log")
    uart = None
    analyzer = None
    try:
        analyzer_settings = None
        if getattr(args, "visa_resource", None):
            analyzer = RohdeSchwarzFSH(args.visa_resource, args.visa_backend, args.analyzer_timeout)
            print(f"[HOST] Analyzer connected: {analyzer.idn}")
            if args.analyzer_preset != "off":
                print(f"[HOST] Applying analyzer preset: {args.analyzer_preset}")
                analyzer.apply_preset(args.analyzer_preset)
                print("[HOST] Analyzer preset complete.")
            analyzer_settings = build_awg_scheduler_analyzer_settings(build_analyzer_settings(args))
            if analyzer.legacy_firmware and analyzer_settings.capture_trace:
                raise RuntimeError(
                    f"{analyzer.trace_capture_unsupported_message()} Requested option: --capture-trace."
                )

        uart = UartCoordinator(
            port=args.serial_port,
            baudrate=args.baudrate,
            timeout_s=args.uart_timeout,
            log=console_log,
            dtr=tristate_arg(args.serial_dtr),
            rts=tristate_arg(args.serial_rts),
        )
        print(f"[HOST] UART connected: {args.serial_port} @ {args.baudrate}")

        settings_files = resolve_xilinx_settings(args)
        extra_cflags = build_awg_scheduler_console_cflags(args)
        if extra_cflags:
            print(
                "[HOST] WARNING: generated firmware override CFLAGS are not applied by XSDB launch; "
                "ensure the selected ELF was built with the expected scheduler console options."
            )

        if not args.skip_make_run:
            rx_before = uart.rx_count
            for item in settings_files:
                print(f"[HOST] Using Xilinx settings: {item}")
            print("[HOST] Launching XSDB reset/download/start for AWG scheduler console...")
            run_xsdb_launch(
                project_dir=script_dir,
                output_dir=output_dir,
                uart=uart,
                timeout_s=args.make_timeout,
                settings_files=settings_files,
                args=args,
            )
            print("[HOST] XSDB launch completed.")

            if uart.rx_count == rx_before:
                print(f"[HOST] Reopening UART {args.serial_port} after programming...")
                uart.reopen()

        execute_awg_scheduler_uploaded_sweep(
            uart=uart,
            console_log=console_log,
            args=args,
            output_dir=output_dir,
            analyzer=analyzer,
            analyzer_settings=analyzer_settings,
            dump_analyzer_state=getattr(args, "dump_analyzer_state", False),
        )
        return 0
    except Exception as exc:
        print(f"[HOST] AWG scheduler console error: {exc}")
        return 1
    finally:
        if uart is not None:
            try:
                uart.close()
            except Exception:
                pass
        if analyzer is not None:
            try:
                analyzer.close()
            except Exception:
                pass
        console_log.close()


def build_phase_noise_settings(
    args: argparse.Namespace,
    base: AnalyzerSettings,
) -> AnalyzerSettings:
    return AnalyzerSettings(
        rbw_hz=args.phase_noise_rbw_hz if args.phase_noise_rbw_hz is not None else base.rbw_hz,
        vbw_hz=args.phase_noise_vbw_hz if args.phase_noise_vbw_hz is not None else base.vbw_hz,
        sweep_count=args.phase_noise_sweep_count if args.phase_noise_sweep_count is not None else max(base.sweep_count, 10),
        trace_mode=args.phase_noise_trace_mode if args.phase_noise_trace_mode is not None else "average",
        detector=args.phase_noise_detector if args.phase_noise_detector is not None else "rms",
        reference_level_dbm=(
            args.phase_noise_reference_level_dbm
            if args.phase_noise_reference_level_dbm is not None
            else base.reference_level_dbm
        ),
        display_range_db=(
            args.phase_noise_display_range_db
            if args.phase_noise_display_range_db is not None
            else base.display_range_db
        ),
        attenuation_auto=base.attenuation_auto,
        preamp_on=base.preamp_on,
        impedance_ohms=base.impedance_ohms,
        capture_trace=True,
    )


def build_dynamic_settings(
    args: argparse.Namespace,
    base: AnalyzerSettings,
) -> AnalyzerSettings:
    return AnalyzerSettings(
        rbw_hz=args.dynamic_rbw_hz if args.dynamic_rbw_hz is not None else base.rbw_hz,
        vbw_hz=args.dynamic_vbw_hz if args.dynamic_vbw_hz is not None else base.vbw_hz,
        sweep_count=args.dynamic_sweep_count if args.dynamic_sweep_count is not None else 1,
        trace_mode=args.dynamic_trace_mode if args.dynamic_trace_mode is not None else "maxhold",
        detector=args.dynamic_detector if args.dynamic_detector is not None else base.detector,
        reference_level_dbm=(
            args.dynamic_reference_level_dbm
            if args.dynamic_reference_level_dbm is not None
            else base.reference_level_dbm
        ),
        display_range_db=(
            args.dynamic_display_range_db
            if args.dynamic_display_range_db is not None
            else base.display_range_db
        ),
        attenuation_auto=base.attenuation_auto,
        preamp_on=base.preamp_on,
        impedance_ohms=base.impedance_ohms,
        capture_trace=False,
    )


def build_phase_noise_requests(args: argparse.Namespace) -> List[PhaseNoiseRequest]:
    if not args.phase_noise_span_hz:
        return []

    carrier_mhz_values = args.phase_noise_carrier_mhz or [400.0]
    requests: List[PhaseNoiseRequest] = []
    next_index = 1
    for carrier_mhz in carrier_mhz_values:
        carrier_hz = carrier_mhz * 1e6
        carrier_label = format_frequency_label_hz(carrier_hz)
        for span_hz in args.phase_noise_span_hz:
            span_label = format_frequency_label_hz(span_hz)
            step = StepSpec(
                group="phase_noise",
                index=next_index,
                name=f"phase_noise_{carrier_label}_span_{span_label}",
                marker="",
                description=(
                    f"Close-in carrier trace at {carrier_mhz:g} MHz "
                    f"with {span_hz:g} Hz span"
                ),
                expected_freq_hz=[carrier_hz],
                span_hz=span_hz,
                search_margin_hz=max(span_hz / 2.0, 1.0),
            )
            requests.append(
                PhaseNoiseRequest(
                    carrier_hz=carrier_hz,
                    span_hz=span_hz,
                    step_spec=step,
                )
            )
            next_index += 1
    return requests


def build_phase_noise_offset_requests(
    args: argparse.Namespace,
    settings: AnalyzerSettings,
) -> List[PhaseNoiseOffsetRequest]:
    if not args.phase_noise_offset_hz:
        return []

    carrier_mhz_values = args.phase_noise_carrier_mhz or [400.0]
    sideband_window_hz = (
        args.phase_noise_window_hz
        if args.phase_noise_window_hz is not None
        else max(10.0 * settings.rbw_hz, 1_000.0)
    )

    requests: List[PhaseNoiseOffsetRequest] = []
    next_index = 1
    for carrier_mhz in carrier_mhz_values:
        carrier_hz = carrier_mhz * 1e6
        carrier_label = format_frequency_label_hz(carrier_hz)
        for offset_hz in sorted(args.phase_noise_offset_hz):
            offset_label = format_frequency_label_hz(offset_hz)
            step = StepSpec(
                group="phase_noise_offset",
                index=next_index,
                name=f"phase_noise_offset_{carrier_label}_{offset_label}",
                marker="",
                description=(
                    f"Marker-only sideband sweep at {carrier_mhz:g} MHz "
                    f"with {offset_hz:g} Hz offset"
                ),
                expected_freq_hz=[carrier_hz],
                span_hz=sideband_window_hz,
                search_margin_hz=max(sideband_window_hz / 2.0, 1.0),
            )
            requests.append(
                PhaseNoiseOffsetRequest(
                    carrier_hz=carrier_hz,
                    offset_hz=offset_hz,
                    sideband_window_hz=sideband_window_hz,
                    step_spec=step,
                )
            )
            next_index += 1
    return requests


def build_dynamic_case_matrix(args: argparse.Namespace) -> List[Tuple[float, float, int, int]]:
    if not args.dynamic_start_mhz and not args.dynamic_stop_mhz and not args.dynamic_dwell_ms:
        return []
    if len(args.dynamic_start_mhz) != len(args.dynamic_stop_mhz):
        raise SystemExit("--dynamic-start-mhz and --dynamic-stop-mhz must be provided the same number of times")
    if not args.dynamic_start_mhz:
        raise SystemExit("Custom dynamic retune configuration requires at least one --dynamic-start-mhz/--dynamic-stop-mhz pair")
    if not args.dynamic_dwell_ms:
        raise SystemExit("Custom dynamic retune configuration requires at least one --dynamic-dwell-ms")
    cases: List[Tuple[float, float, int, int]] = []
    for start_mhz, stop_mhz in zip(args.dynamic_start_mhz, args.dynamic_stop_mhz):
        for dwell_ms in args.dynamic_dwell_ms:
            transitions = max(1, int(round(args.dynamic_active_ms / float(dwell_ms))))
            cases.append((start_mhz * 1e6, stop_mhz * 1e6, dwell_ms, transitions))
    if len(cases) > MAX_DYNAMIC_CASES:
        raise SystemExit(
            f"Custom dynamic retune configuration expands to {len(cases)} cases; "
            f"the current firmware/host limit is {MAX_DYNAMIC_CASES}"
        )
    return cases


def build_dynamic_specs(args: argparse.Namespace) -> List[DynamicRetuneSpec]:
    custom_cases = build_dynamic_case_matrix(args)
    if custom_cases:
        specs: List[DynamicRetuneSpec] = []
        total = len(custom_cases)
        for index, (start_hz, stop_hz, dwell_ms, transitions) in enumerate(custom_cases, start=1):
            start_label = str(int(round(start_hz / 1e6)))
            stop_label = str(int(round(stop_hz / 1e6)))
            step_name = f"dynamic_toggle_{start_label}_{stop_label}_{dwell_ms}ms"
            marker_name = f"toggle_{start_label}_to_{stop_label}_{dwell_ms}ms"
            specs.append(
                DynamicRetuneSpec(
                    index=index,
                    name=step_name,
                    marker=f"[DYNAMIC-SFDR] Step {index}/{total}: {marker_name}.",
                    done_marker=f"[DYNAMIC-SFDR] Completed burst {index}/{total}.",
                    description=(
                        f"Rapid DDS retune burst toggling {start_hz / 1e6:g} MHz <-> "
                        f"{stop_hz / 1e6:g} MHz with {dwell_ms} ms dwell"
                    ),
                    intended_freq_hz=[start_hz, stop_hz],
                    dwell_ms=dwell_ms,
                    transitions=transitions,
                    intended_margin_hz=args.dynamic_intended_margin_hz,
                )
            )
        return specs

    specs: List[DynamicRetuneSpec] = []
    for item in DYNAMIC_RETUNE_STEP_SPECS:
        specs.append(
            DynamicRetuneSpec(
                index=item.index,
                name=item.name,
                marker=item.marker,
                done_marker=item.done_marker,
                description=item.description,
                intended_freq_hz=list(item.intended_freq_hz),
                dwell_ms=item.dwell_ms,
                transitions=item.transitions,
                intended_margin_hz=args.dynamic_intended_margin_hz,
            )
        )
    return specs


def advance_boot_defaults_or_wait_for_nco(
    uart: UartCoordinator,
    clock_reply: str,
    rate_reply: str,
    timeout_s: float,
) -> bool:
    deadline = time.monotonic() + timeout_s
    stage = "clock"

    while time.monotonic() < deadline:
        remaining = max(0.1, deadline - time.monotonic())
        seen = uart.wait_for(
            NCO_START_PROMPT,
            remaining,
            extra_needles=[
                CLOCK_CONFIG_PROMPT,
                BOOT_RATE_PROMPT,
                BOOT_CHOICE_PROMPT,
            ],
        )

        if seen == NCO_START_PROMPT:
            return True

        if seen == CLOCK_CONFIG_PROMPT:
            stage = "clock"
            continue

        if seen == BOOT_RATE_PROMPT:
            stage = "rate"
            continue

        if stage == "clock":
            uart.send_line(clock_reply)
            stage = "rate"
        else:
            uart.send_line(rate_reply)
            return False

        time.sleep(BOOT_REPLY_PACING_S)

    raise TimeoutError(f"Timed out waiting for UART text: {NCO_START_PROMPT!r}")


def resolve_xilinx_settings(args: argparse.Namespace) -> List[Path]:
    return [Path(item) for item in args.xilinx_settings]


def resolve_xsdb_elf_path(project_dir: Path, args: argparse.Namespace) -> Path:
    if args.xsdb_elf:
        return Path(args.xsdb_elf)
    return project_dir / "build" / "fmcdac.elf"


def resolve_xsdb_xsa_path(project_dir: Path, args: argparse.Namespace) -> Path:
    if args.xsdb_xsa:
        return Path(args.xsdb_xsa)
    return project_dir / "build" / "tmp" / "system_top.xsa"


def tcl_quote_path(path: Path) -> str:
    return str(path.resolve()).replace("\\", "/")


def build_xsdb_script_text(
    *,
    hw_url: str,
    target_filter: str,
    elf_path: Path,
    xsa_path: Optional[Path],
    reset_delay_ms: int,
) -> str:
    lines = [
        f"connect -url {hw_url}",
        f"targets -set -nocase -filter {{{target_filter}}}",
    ]
    if xsa_path is not None:
        lines.append(f"loadhw -hw {tcl_quote_path(xsa_path)} -regs")
        lines.append("configparams mdm-detect-bscan-mask 2")
        lines.append(f"targets -set -nocase -filter {{{target_filter}}}")
    lines.extend(
        [
            "rst -system",
            f"after {reset_delay_ms}",
            f"targets -set -nocase -filter {{{target_filter}}}",
            f"dow {tcl_quote_path(elf_path)}",
            f"targets -set -nocase -filter {{{target_filter}}}",
            "con",
            "exit",
        ]
    )
    return "\n".join(lines) + "\n"


def build_xsdb_launch_command(
    settings_files: List[Path],
    xsdb_exe: str,
    script_path: Path,
) -> str:
    parts: List[str] = []
    for item in settings_files:
        if not item.is_file():
            raise RuntimeError(f"Xilinx settings file not found: {item}")
        parts.append(f'call "{item}"')
    parts.append(f'"{xsdb_exe}" "{script_path.resolve()}"')
    return " && ".join(parts)


def run_make_launch(
    project_dir: Path,
    timeout_s: float,
    settings_files: List[Path],
    cflags: str,
    refresh_build: bool,
    uart: Optional[UartCoordinator] = None,
) -> None:
    lines: List[str] = ["@echo off"]
    for item in settings_files:
        if not item.is_file():
            raise RuntimeError(f"Xilinx settings file not found: {item}")
        lines.append(f'call "{item}"')
    if refresh_build:
        lines.extend(["make update", "make clean"])
    lines.append("make run")
    script_path = project_dir / "build" / "host_make_launch.bat"
    script_path.parent.mkdir(parents=True, exist_ok=True)
    script_path.write_text("\r\n".join(lines) + "\r\n", encoding="utf-8")
    env = os.environ.copy()
    env["NEW_CFLAGS"] = cflags
    try:
        proc = subprocess.Popen(
            ["cmd.exe", "/d", "/c", str(script_path)],
            cwd=str(project_dir),
            env=env,
        )
    except FileNotFoundError as exc:  # pragma: no cover - Windows-specific
        raise RuntimeError("Could not find 'cmd.exe' or make in PATH") from exc

    deadline = time.monotonic() + timeout_s
    while True:
        if uart is not None:
            uart.pump()
        ret = proc.poll()
        if ret is not None:
            if uart is not None:
                while uart.pump():
                    pass
            if ret != 0:
                raise RuntimeError(f"'make run' failed with exit code {ret}")
            return
        if time.monotonic() >= deadline:
            proc.kill()
            raise TimeoutError(f"'make run' exceeded timeout of {timeout_s:.0f} seconds")
        time.sleep(HOST_POLL_SLEEP_S)


def run_xsdb_launch(
    project_dir: Path,
    output_dir: Path,
    uart: UartCoordinator,
    timeout_s: float,
    settings_files: List[Path],
    args: argparse.Namespace,
) -> None:
    elf_path = resolve_xsdb_elf_path(project_dir, args)
    if not elf_path.is_file():
        raise RuntimeError(f"XSDB ELF not found: {elf_path}")

    xsa_path: Optional[Path] = None
    if not args.xsdb_skip_loadhw:
        xsa_path = resolve_xsdb_xsa_path(project_dir, args)
        if not xsa_path.is_file():
            raise RuntimeError(
                f"XSDB XSA not found: {xsa_path}. Use --xsdb-xsa to point to the intended hardware export "
                "or --xsdb-skip-loadhw to bypass loadhw."
            )

    script_path = output_dir / "launch_xsdb.tcl"
    script_path.write_text(
        build_xsdb_script_text(
            hw_url=args.xsdb_hw_url,
            target_filter=args.xsdb_target_filter,
            elf_path=elf_path,
            xsa_path=xsa_path,
            reset_delay_ms=args.xsdb_reset_delay_ms,
        ),
        encoding="utf-8",
    )

    command = build_xsdb_launch_command(
        settings_files,
        args.xsdb_exe,
        script_path,
    )
    try:
        proc = subprocess.Popen(
            ["cmd.exe", "/d", "/c", command],
            cwd=str(project_dir),
        )
    except FileNotFoundError as exc:  # pragma: no cover - Windows-specific
        raise RuntimeError("Could not find 'cmd.exe' or the requested XSDB executable in PATH") from exc

    deadline = time.monotonic() + timeout_s
    while True:
        uart.pump()
        ret = proc.poll()
        if ret is not None:
            while uart.pump():
                pass
            if ret != 0:
                raise RuntimeError(f"XSDB launch failed with exit code {ret}")
            return
        if time.monotonic() >= deadline:
            proc.kill()
            raise TimeoutError(f"XSDB launch exceeded timeout of {timeout_s:.0f} seconds")
        time.sleep(HOST_POLL_SLEEP_S)


def capture_step_group(
    uart: UartCoordinator,
    analyzer: RohdeSchwarzFSH,
    output_dir: Path,
    step_specs: Sequence[StepSpec],
    done_marker: str,
    timeout_s: float,
    settings: AnalyzerSettings,
    benchmark_prompts_enabled: bool,
    dump_analyzer_state: bool = False,
    write_step_csv: bool = False,
    settle_timeout_s: float = 0.0,
    settle_error_hz: float = 0.0,
    use_known_tone_capture: bool = False,
    lock_to_expected: bool = False,
) -> List[StepCaptureSummary]:
    summaries: List[StepCaptureSummary] = []
    reference_power_dbm: Optional[float] = None
    reference_step_name: Optional[str] = None

    for step in step_specs:
        uart.wait_for(step.marker, timeout_s)
        if benchmark_prompts_enabled:
            uart.wait_for(CONTINUE_PROMPT, timeout_s)

        if use_known_tone_capture:
            summary = capture_known_tone_step(
                analyzer=analyzer,
                output_dir=output_dir,
                step=step,
                settings=settings,
                dump_analyzer_state=dump_analyzer_state,
                write_csv=write_step_csv,
                write_json=dump_analyzer_state,
                settle_timeout_s=settle_timeout_s,
                settle_error_hz=settle_error_hz,
                lock_to_expected=lock_to_expected,
            )
        else:
            summary = capture_trace_step(
                analyzer=analyzer,
                output_dir=output_dir,
                step=step,
                settings=settings,
                dump_analyzer_state=dump_analyzer_state,
                write_csv=write_step_csv,
                write_json=dump_analyzer_state,
                settle_timeout_s=settle_timeout_s,
                settle_error_hz=settle_error_hz,
            )
        metrics = summary.metrics

        if reference_power_dbm is None:
            reference_power_dbm = metrics.power_dbm
            reference_step_name = step.name

        metrics.reference_power_dbm = reference_power_dbm
        metrics.reference_step_name = reference_step_name
        metrics.power_delta_db = metrics.power_dbm - reference_power_dbm
        summaries.append(summary)
        print_step_summary(step, metrics)
        uart.send_line()

    uart.wait_for(done_marker, timeout_s)
    return summaries


def capture_sfdr_group(
    uart: UartCoordinator,
    analyzer: RohdeSchwarzFSH,
    output_dir: Path,
    step_specs: Sequence[StepSpec],
    done_marker: str,
    timeout_s: float,
    settings: AnalyzerSettings,
    sfdr_settings: SfdrSettings,
    phase_noise_requests: Sequence[PhaseNoiseRequest],
    phase_noise_offset_requests: Sequence[PhaseNoiseOffsetRequest],
    phase_noise_settings: AnalyzerSettings,
    benchmark_prompts_enabled: bool,
    dump_analyzer_state: bool = False,
    write_step_csv: bool = False,
    settle_timeout_s: float = 0.0,
    settle_error_hz: float = 0.0,
) -> List[StepCaptureSummary]:
    summaries: List[StepCaptureSummary] = []

    for step in step_specs:
        uart.wait_for(step.marker, timeout_s)
        if benchmark_prompts_enabled:
            uart.wait_for(CONTINUE_PROMPT, timeout_s)

        _, _, metrics = capture_with_frequency_settle(
            lambda: ([0.0], [0.0], analyzer.capture_sfdr(step, settings, sfdr_settings)),
            step,
            timeout_s=settle_timeout_s,
            error_hz=settle_error_hz,
        )
        json_path = output_dir / f"step{step.index:02d}_{step.name}.json"
        csv_path = output_dir / f"step{step.index:02d}_{step.name}.csv"
        if write_step_csv:
            csv_path.write_text(
                "marker_type,frequency_hz,level_dbm\n"
                f"carrier,{metrics.power_freq_hz:.6f},{metrics.power_dbm:.6f}\n"
                f"left_spur,{'' if metrics.left_spur_freq_hz is None else f'{metrics.left_spur_freq_hz:.6f}'},{'' if metrics.left_spur_power_dbm is None else f'{metrics.left_spur_power_dbm:.6f}'}\n"
                f"right_spur,{'' if metrics.right_spur_freq_hz is None else f'{metrics.right_spur_freq_hz:.6f}'},{'' if metrics.right_spur_power_dbm is None else f'{metrics.right_spur_power_dbm:.6f}'}\n"
                f"worst_spur,{'' if metrics.spur_freq_hz is None else f'{metrics.spur_freq_hz:.6f}'},{'' if metrics.spur_power_dbm is None else f'{metrics.spur_power_dbm:.6f}'}\n",
                encoding="utf-8",
            )
        if dump_analyzer_state:
            write_step_json(
                json_path,
                analyzer.idn,
                step,
                metrics,
                extra=build_step_extra(
                    analyzer,
                    dump_analyzer_state,
                    {"sfdr_settings": asdict(sfdr_settings)},
                ),
            )

        summary = StepCaptureSummary(
            group=step.group,
            step_index=step.index,
            name=step.name,
            marker=step.marker,
            description=step.description,
            expected_freq_hz=step.expected_freq_hz,
            csv_path=str(csv_path.resolve()) if write_step_csv else "",
            metrics=metrics,
        )
        summaries.append(summary)
        print_step_summary(step, metrics)

        matching_phase_noise = [
            request
            for request in phase_noise_requests
            if abs(request.carrier_hz - step.expected_freq_hz[0]) <= 1.0
        ]
        for request in matching_phase_noise:
            phase_summary = capture_trace_step(
                analyzer=analyzer,
                output_dir=output_dir,
                step=request.step_spec,
                settings=phase_noise_settings,
                dump_analyzer_state=dump_analyzer_state,
                write_csv=write_step_csv,
                write_json=dump_analyzer_state,
            )
            summaries.append(phase_summary)
            print_step_summary(request.step_spec, phase_summary.metrics)

        matching_phase_noise_offsets = [
            request
            for request in phase_noise_offset_requests
            if abs(request.carrier_hz - step.expected_freq_hz[0]) <= 1.0
        ]
        for request in matching_phase_noise_offsets:
            phase_offset_summary = capture_phase_noise_offset_step(
                analyzer=analyzer,
                output_dir=output_dir,
                request=request,
                settings=phase_noise_settings,
                carrier_power_dbm=metrics.power_dbm,
                carrier_freq_hz=metrics.power_freq_hz,
                dump_analyzer_state=dump_analyzer_state,
                write_step_csv=write_step_csv,
            )
            summaries.append(phase_offset_summary)
            print_phase_noise_offset_summary(request, phase_offset_summary.metrics)

        uart.send_line()

    uart.wait_for(done_marker, timeout_s)
    return summaries


def average_linear_power_dbm(powers_dbm: Sequence[Optional[float]]) -> Optional[float]:
    usable = [item for item in powers_dbm if item is not None]
    if not usable:
        return None
    linear_avg = sum(10.0 ** (item / 10.0) for item in usable) / float(len(usable))
    return 10.0 * math.log10(linear_avg)


def capture_phase_noise_offset_metrics(
    analyzer: RohdeSchwarzFSH,
    request: PhaseNoiseOffsetRequest,
    settings: AnalyzerSettings,
    carrier_power_dbm: float,
    carrier_freq_hz: float,
) -> PhaseNoiseOffsetMetrics:
    half_window_hz = request.sideband_window_hz / 2.0
    sidebands: List[WindowPeak] = []

    for label, target_hz in (
        ("left", carrier_freq_hz - request.offset_hz),
        ("right", carrier_freq_hz + request.offset_hz),
    ):
        center_hz = target_hz
        span_hz = max(request.sideband_window_hz, 1.0)
        power_dbm, freq_hz = analyzer._capture_marker_at_frequency(
            center_hz=center_hz,
            span_hz=span_hz,
            marker_hz=target_hz,
            settings=settings,
        )
        sidebands.append(
            WindowPeak(
                label=label,
                search_left_hz=target_hz - half_window_hz,
                search_right_hz=target_hz + half_window_hz,
                power_dbm=power_dbm,
                freq_hz=freq_hz,
            )
        )

    avg_sideband_power_dbm = average_linear_power_dbm(
        [item.power_dbm for item in sidebands]
    )
    avg_sideband_dbc = None
    avg_sideband_dbc_per_hz = None
    if avg_sideband_power_dbm is not None:
        avg_sideband_dbc = avg_sideband_power_dbm - carrier_power_dbm
        avg_sideband_dbc_per_hz = avg_sideband_dbc - (10.0 * math.log10(max(settings.rbw_hz, 1.0)))

    return PhaseNoiseOffsetMetrics(
        carrier_power_dbm=carrier_power_dbm,
        carrier_freq_hz=carrier_freq_hz,
        offset_hz=request.offset_hz,
        sideband_window_hz=request.sideband_window_hz,
        rbw_hz=settings.rbw_hz,
        vbw_hz=settings.vbw_hz,
        sweep_count=settings.sweep_count,
        trace_mode=settings.trace_mode,
        detector=settings.detector,
        reference_level_dbm=settings.reference_level_dbm,
        display_range_db=settings.display_range_db,
        attenuation_auto=settings.attenuation_auto,
        preamp_on=settings.preamp_on,
        impedance_ohms=settings.impedance_ohms,
        sidebands=sidebands,
        avg_sideband_power_dbm=avg_sideband_power_dbm,
        avg_sideband_dbc=avg_sideband_dbc,
        avg_sideband_dbc_per_hz=avg_sideband_dbc_per_hz,
    )


def capture_phase_noise_offset_step(
    analyzer: RohdeSchwarzFSH,
    output_dir: Path,
    request: PhaseNoiseOffsetRequest,
    settings: AnalyzerSettings,
    carrier_power_dbm: float,
    carrier_freq_hz: float,
    dump_analyzer_state: bool = False,
    write_step_csv: bool = False,
) -> StepCaptureSummary:
    metrics = capture_phase_noise_offset_metrics(
        analyzer=analyzer,
        request=request,
        settings=settings,
        carrier_power_dbm=carrier_power_dbm,
        carrier_freq_hz=carrier_freq_hz,
    )
    step = request.step_spec
    csv_path = output_dir / f"step{step.index:02d}_{step.name}.csv"
    json_path = output_dir / f"step{step.index:02d}_{step.name}.json"

    lines = ["label,target_left_hz,target_right_hz,measured_freq_hz,measured_power_dbm"]
    for sideband in metrics.sidebands:
        lines.append(
            f"{sideband.label},"
            f"{sideband.search_left_hz:.6f},"
            f"{sideband.search_right_hz:.6f},"
            f"{'' if sideband.freq_hz is None else f'{sideband.freq_hz:.6f}'},"
            f"{'' if sideband.power_dbm is None else f'{sideband.power_dbm:.6f}'}"
        )
    if write_step_csv:
        csv_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    if dump_analyzer_state:
        write_step_json(
            json_path,
            analyzer.idn,
            step,
            metrics,
            extra=build_step_extra(analyzer, dump_analyzer_state),
        )

    return StepCaptureSummary(
        group="phase_noise_offset",
        step_index=step.index,
        name=step.name,
        marker=step.marker,
        description=step.description,
        expected_freq_hz=step.expected_freq_hz,
        csv_path=str(csv_path.resolve()) if write_step_csv else "",
        metrics=metrics,
    )


def print_phase_noise_offset_summary(
    request: PhaseNoiseOffsetRequest,
    metrics: PhaseNoiseOffsetMetrics,
) -> None:
    sideband_parts: List[str] = []
    for item in metrics.sidebands:
        if item.power_dbm is None or item.freq_hz is None:
            sideband_parts.append(f"{item.label}=n/a")
        else:
            sideband_parts.append(
                f"{item.label}={item.power_dbm:.3f} dBm @ {item.freq_hz / 1e6:.6f} MHz"
            )

    avg_text = "avg=n/a"
    if metrics.avg_sideband_power_dbm is not None and metrics.avg_sideband_dbc is not None:
        avg_text = f"avg={metrics.avg_sideband_power_dbm:.3f} dBm ({metrics.avg_sideband_dbc:.3f} dBc)"

    density_text = "density=n/a"
    if metrics.avg_sideband_dbc_per_hz is not None:
        density_text = f"density={metrics.avg_sideband_dbc_per_hz:.3f} dBc/Hz"

    print(
        f"[HOST] {request.step_spec.name}: carrier={metrics.carrier_power_dbm:.3f} dBm @ "
        f"{metrics.carrier_freq_hz / 1e6:.6f} MHz, offset={metrics.offset_hz:.0f} Hz, "
        + ", ".join(sideband_parts)
        + f", {avg_text}, {density_text}"
    )


def build_spur_search_windows(
    expected_freq_hz: Sequence[float],
    search_start_hz: float,
    search_stop_hz: float,
    guard_hz: float,
) -> List[Tuple[str, float, float]]:
    excluded: List[Tuple[float, float]] = []
    for freq_hz in sorted(expected_freq_hz):
        left_hz = max(search_start_hz, freq_hz - guard_hz)
        right_hz = min(search_stop_hz, freq_hz + guard_hz)
        if right_hz <= left_hz:
            continue
        excluded.append((left_hz, right_hz))

    if not excluded:
        return [("spur_window_1", search_start_hz, search_stop_hz)]

    windows: List[Tuple[str, float, float]] = []
    current_left_hz = search_start_hz
    window_index = 1
    for left_hz, right_hz in excluded:
        if left_hz > current_left_hz:
            windows.append((f"spur_window_{window_index}", current_left_hz, left_hz))
            window_index += 1
        current_left_hz = max(current_left_hz, right_hz)
    if search_stop_hz > current_left_hz:
        windows.append((f"spur_window_{window_index}", current_left_hz, search_stop_hz))
    return windows


def capture_dynamic_retune_metrics(
    analyzer: RohdeSchwarzFSH,
    step: DynamicRetuneSpec,
    settings: AnalyzerSettings,
    sfdr_settings: SfdrSettings,
    measured_elapsed_us: Optional[int] = None,
) -> DynamicRetuneMetrics:
    exclusion_guard_hz = max(sfdr_settings.carrier_guard_hz, step.intended_margin_hz)
    intended_peaks: List[WindowPeak] = []
    for freq_hz in step.intended_freq_hz:
        left_hz = max(sfdr_settings.search_start_hz, freq_hz - step.intended_margin_hz)
        right_hz = min(sfdr_settings.search_stop_hz, freq_hz + step.intended_margin_hz)
        span_hz = max(right_hz - left_hz, 1.0)
        power_dbm, peak_freq_hz = analyzer._capture_marker_at_frequency(
            freq_hz,
            span_hz,
            freq_hz,
            settings,
        )
        intended_peaks.append(
            WindowPeak(
                label=f"intended_{int(freq_hz / 1e6)}mhz",
                search_left_hz=left_hz,
                search_right_hz=right_hz,
                power_dbm=power_dbm,
                freq_hz=peak_freq_hz,
            )
        )

    reference_power_dbm = None
    reference_freq_hz = None
    for peak in intended_peaks:
        if peak.power_dbm is None or peak.freq_hz is None:
            continue
        if reference_power_dbm is None or peak.power_dbm > reference_power_dbm:
            reference_power_dbm = peak.power_dbm
            reference_freq_hz = peak.freq_hz

    unintended_peaks: List[WindowPeak] = []
    for label, left_hz, right_hz in build_spur_search_windows(
        step.intended_freq_hz,
        sfdr_settings.search_start_hz,
        sfdr_settings.search_stop_hz,
        exclusion_guard_hz,
    ):
        center_hz = (left_hz + right_hz) / 2.0
        span_hz = max(right_hz - left_hz, 1.0)
        power_dbm, peak_freq_hz = analyzer._capture_peak_for_span(center_hz, span_hz, settings)
        unintended_peaks.append(
            WindowPeak(
                label=label,
                search_left_hz=left_hz,
                search_right_hz=right_hz,
                power_dbm=power_dbm,
                freq_hz=peak_freq_hz,
            )
        )

    spur_power_dbm = None
    spur_freq_hz = None
    for peak in unintended_peaks:
        if peak.power_dbm is None or peak.freq_hz is None:
            continue
        if spur_power_dbm is None or peak.power_dbm > spur_power_dbm:
            spur_power_dbm = peak.power_dbm
            spur_freq_hz = peak.freq_hz

    dynamic_spur_margin_db = None
    if reference_power_dbm is not None and spur_power_dbm is not None:
        dynamic_spur_margin_db = reference_power_dbm - spur_power_dbm
    measured_us_per_transition = None
    if measured_elapsed_us is not None and step.transitions > 0:
        measured_us_per_transition = float(measured_elapsed_us) / float(step.transitions)

    return DynamicRetuneMetrics(
        dwell_ms=step.dwell_ms,
        transitions=step.transitions,
        active_duration_ms=step.dwell_ms * step.transitions,
        search_left_hz=sfdr_settings.search_start_hz,
        search_right_hz=sfdr_settings.search_stop_hz,
        carrier_guard_hz=exclusion_guard_hz,
        intended_margin_hz=step.intended_margin_hz,
        rbw_hz=settings.rbw_hz,
        vbw_hz=settings.vbw_hz,
        sweep_count=settings.sweep_count,
        trace_mode=settings.trace_mode,
        detector=settings.detector,
        reference_level_dbm=settings.reference_level_dbm,
        display_range_db=settings.display_range_db,
        attenuation_auto=settings.attenuation_auto,
        preamp_on=settings.preamp_on,
        impedance_ohms=settings.impedance_ohms,
        intended_peaks=intended_peaks,
        unintended_peaks=unintended_peaks,
        measured_elapsed_us=measured_elapsed_us,
        measured_us_per_transition=measured_us_per_transition,
        reference_power_dbm=reference_power_dbm,
        reference_freq_hz=reference_freq_hz,
        spur_power_dbm=spur_power_dbm,
        spur_freq_hz=spur_freq_hz,
        dynamic_spur_margin_db=dynamic_spur_margin_db,
    )


def print_dynamic_summary(step: DynamicRetuneSpec, metrics: DynamicRetuneMetrics) -> None:
    intended_parts: List[str] = []
    for peak in metrics.intended_peaks:
        if peak.power_dbm is None or peak.freq_hz is None:
            intended_parts.append(f"{peak.label}=n/a")
            continue
        intended_parts.append(
            f"{peak.label}={peak.power_dbm:.3f} dBm @ {peak.freq_hz / 1e6:.6f} MHz"
        )

    spur_text = "spur=n/a"
    if metrics.spur_power_dbm is not None and metrics.spur_freq_hz is not None:
        spur_text = f"spur={metrics.spur_power_dbm:.3f} dBm @ {metrics.spur_freq_hz / 1e6:.6f} MHz"

    margin_text = "margin=n/a"
    if metrics.dynamic_spur_margin_db is not None:
        margin_text = f"margin={metrics.dynamic_spur_margin_db:.3f} dB"

    elapsed_text = "elapsed=n/a"
    if metrics.measured_elapsed_us is not None:
        elapsed_text = f"elapsed={metrics.measured_elapsed_us} us"
        if metrics.measured_us_per_transition is not None:
            elapsed_text += f" ({metrics.measured_us_per_transition:.3f} us/transition)"

    print(
        f"[HOST] {step.name}: dwell_ms={metrics.dwell_ms}, "
        f"transitions={metrics.transitions}, "
        f"active_ms~={metrics.active_duration_ms}, "
        + ", ".join(intended_parts)
        + f", {spur_text}, {margin_text}, {elapsed_text}"
    )


def parse_dynamic_elapsed_us(line: str) -> Optional[int]:
    match = re.search(r"\belapsed_us=(\d+)\b", line)
    if not match:
        return None
    return int(match.group(1))


def capture_dynamic_sfdr_group(
    uart: UartCoordinator,
    analyzer: RohdeSchwarzFSH,
    output_dir: Path,
    step_specs: Sequence[DynamicRetuneSpec],
    done_marker: str,
    timeout_s: float,
    settings: AnalyzerSettings,
    sfdr_settings: SfdrSettings,
    benchmark_prompts_enabled: bool,
    dump_analyzer_state: bool = False,
    write_step_csv: bool = False,
) -> List[StepCaptureSummary]:
    summaries: List[StepCaptureSummary] = []

    for step in step_specs:
        uart.wait_for(step.marker, timeout_s)
        if benchmark_prompts_enabled:
            uart.wait_for(CONTINUE_PROMPT, timeout_s)
        uart.send_line()

        metrics = capture_dynamic_retune_metrics(analyzer, step, settings, sfdr_settings)
        csv_path = output_dir / f"step{step.index:02d}_{step.name}.csv"
        json_path = output_dir / f"step{step.index:02d}_{step.name}.json"

        lines = ["label,search_left_hz,search_right_hz,peak_freq_hz,peak_power_dbm"]
        for peak in metrics.intended_peaks + metrics.unintended_peaks:
            lines.append(
                f"{peak.label},"
                f"{peak.search_left_hz:.6f},"
                f"{peak.search_right_hz:.6f},"
                f"{'' if peak.freq_hz is None else f'{peak.freq_hz:.6f}'},"
                f"{'' if peak.power_dbm is None else f'{peak.power_dbm:.6f}'}"
            )
        if write_step_csv:
            csv_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        if dump_analyzer_state:
            write_step_json(
                json_path,
                analyzer.idn,
                StepSpec(
                    group="dynamic_sfdr",
                    index=step.index,
                    name=step.name,
                    marker=step.marker,
                    description=step.description,
                    expected_freq_hz=list(step.intended_freq_hz),
                    span_hz=max(sfdr_settings.search_stop_hz - sfdr_settings.search_start_hz, 1.0),
                    search_margin_hz=step.intended_margin_hz,
                ),
                metrics,
                extra=build_step_extra(
                    analyzer,
                    dump_analyzer_state,
                    {"intended_freq_hz": step.intended_freq_hz, "sfdr_settings": asdict(sfdr_settings)},
                ),
            )

        summary = StepCaptureSummary(
            group="dynamic_sfdr",
            step_index=step.index,
            name=step.name,
            marker=step.marker,
            description=step.description,
            expected_freq_hz=list(step.intended_freq_hz),
            csv_path=str(csv_path.resolve()) if write_step_csv else "",
            metrics=metrics,
        )
        summaries.append(summary)
        done_line = uart.wait_for_line_containing(step.done_marker, timeout_s)
        measured_elapsed_us = parse_dynamic_elapsed_us(done_line)
        if measured_elapsed_us is not None:
            metrics.measured_elapsed_us = measured_elapsed_us
            metrics.measured_us_per_transition = (
                float(measured_elapsed_us) / float(step.transitions)
                if step.transitions > 0
                else None
            )
        print_dynamic_summary(step, metrics)

    uart.wait_for(done_marker, timeout_s)
    return summaries


def wait_for_optional_prompt(
    uart: UartCoordinator,
    prompt: str,
    timeout_s: float,
    extra_needles: Sequence[str],
) -> Optional[str]:
    try:
        return uart.wait_for(prompt, timeout_s, extra_needles=extra_needles)
    except TimeoutError:
        return None


def parse_throughput_results(text: str) -> List[dict]:
    results: List[dict] = []
    for line in text.splitlines():
        if "[THROUGHPUT] RESULT " not in line:
            continue
        payload = line.split("[THROUGHPUT] RESULT ", 1)[1].strip()
        fields = {}
        for item in payload.split():
            if "=" not in item:
                continue
            key, value = item.split("=", 1)
            fields[key] = value
        if {"name", "ops", "total_us", "ops_per_sec", "ns_per_op"} - fields.keys():
            continue
        results.append(
            {
                "name": fields["name"],
                "ops": int(fields["ops"]),
                "total_us": int(fields["total_us"]),
                "ops_per_sec": int(fields["ops_per_sec"]),
                "ns_per_op": int(fields["ns_per_op"]),
            }
        )
    return results


def parse_throughput_results_from_log(log_path: Path) -> List[dict]:
    if not log_path.is_file():
        return []
    return parse_throughput_results(log_path.read_text(encoding="utf-8", errors="replace"))


def run_uart_rtt_benchmark(
    uart: UartCoordinator,
    timeout_s: float,
    samples: int,
) -> dict:
    rtts_us: List[float] = []

    uart.wait_for(UART_RTT_READY_MARKER, timeout_s)
    for index in range(samples):
        token = f"{index:03d}"
        start_ns = time.perf_counter_ns()
        uart.send_line(f"PING {token}")
        uart.wait_for(f"[UART-RTT] PONG {token}", timeout_s)
        elapsed_us = (time.perf_counter_ns() - start_ns) / 1000.0
        rtts_us.append(elapsed_us)
        print(f"[HOST] uart_rtt_{token}: RTT={elapsed_us:.3f} us")

    uart.send_line("DONE")
    uart.wait_for(UART_RTT_DONE_MARKER, timeout_s)

    avg_us = sum(rtts_us) / float(len(rtts_us))
    result = {
        "samples": len(rtts_us),
        "min_rtt_us": min(rtts_us),
        "max_rtt_us": max(rtts_us),
        "avg_rtt_us": avg_us,
        "avg_one_way_estimate_us": avg_us / 2.0,
        "rtts_us": rtts_us,
    }
    print(
        "[HOST] UART RTT summary: "
        f"min={result['min_rtt_us']:.3f} us, "
        f"avg={result['avg_rtt_us']:.3f} us, "
        f"max={result['max_rtt_us']:.3f} us, "
        f"one_way_est~={result['avg_one_way_estimate_us']:.3f} us"
    )
    return result


def write_dds_band_artifacts(summary_path: Path, output_dir: Path, label: str) -> None:
    write_single_tone_band_artifacts(
        summary_path,
        output_dir,
        label,
        groups=("dds_band",),
        title="DDS Band Level Delta vs Frequency",
        stem="dds_band_plot",
    )


def write_single_tone_band_artifacts(
    summary_path: Path,
    output_dir: Path,
    label: str,
    *,
    groups: Sequence[str],
    title: str,
    stem: str,
) -> None:
    try:
        series = load_dds_band_series(summary_path, label, groups=groups)
    except Exception:
        return

    if not series.points:
        return

    write_dds_band_csv([series], output_dir / f"{stem}.csv")
    write_dds_band_svg([series], output_dir / f"{stem}.svg", title)


def write_sfdr_results_csv(steps: Sequence[StepCaptureSummary], output_path: Path) -> None:
    sfdr_steps = [step for step in steps if step.group == "sfdr" and step.metrics.sfdr_db is not None]
    if not sfdr_steps:
        return

    lines = [
        "frequency_mhz,carrier_power_dbm,carrier_freq_mhz,worst_spur_power_dbm,worst_spur_freq_mhz,sfdr_db"
    ]
    for step in sorted(sfdr_steps, key=lambda item: item.step_index):
        metrics = step.metrics
        lines.append(
            f"{step.expected_freq_hz[0] / 1e6:.6f},"
            f"{metrics.power_dbm:.6f},"
            f"{metrics.power_freq_hz / 1e6:.6f},"
            f"{'' if metrics.spur_power_dbm is None else f'{metrics.spur_power_dbm:.6f}'},"
            f"{'' if metrics.spur_freq_hz is None else f'{metrics.spur_freq_hz / 1e6:.6f}'},"
            f"{metrics.sfdr_db:.6f}"
        )

    output_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_phase_noise_results_csv(steps: Sequence[StepCaptureSummary], output_path: Path) -> None:
    phase_steps = [step for step in steps if step.group == "phase_noise"]
    if not phase_steps:
        return

    lines = [
        "carrier_mhz,span_hz,peak_power_dbm,peak_freq_mhz,freq_error_hz,rbw_hz,vbw_hz,sweep_count,trace_points,trace_capture_degraded,trace_capture_error,csv_path"
    ]
    for step in sorted(
        phase_steps,
        key=lambda item: (item.expected_freq_hz[0], item.metrics.span_hz, item.step_index),
    ):
        metrics = step.metrics
        expected_hz = step.expected_freq_hz[0]
        freq_error_hz = metrics.power_freq_hz - expected_hz
        lines.append(
            f"{expected_hz / 1e6:.6f},"
            f"{metrics.span_hz:.6f},"
            f"{metrics.power_dbm:.6f},"
            f"{metrics.power_freq_hz / 1e6:.6f},"
            f"{freq_error_hz:.6f},"
            f"{metrics.rbw_hz:.6f},"
            f"{metrics.vbw_hz:.6f},"
            f"{metrics.sweep_count},"
            f"{metrics.trace_points},"
            f"{str(metrics.trace_capture_degraded).lower()},"
            f"\"{'' if metrics.trace_capture_error is None else metrics.trace_capture_error.replace('\"', '\"\"')}\","
            f"{step.csv_path}"
        )

    output_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_phase_noise_offset_results_csv(steps: Sequence[StepCaptureSummary], output_path: Path) -> None:
    phase_steps = [step for step in steps if step.group == "phase_noise_offset"]
    if not phase_steps:
        return

    lines = [
        "carrier_mhz,offset_hz,carrier_power_dbm,carrier_freq_mhz,left_power_dbm,left_freq_mhz,right_power_dbm,right_freq_mhz,avg_sideband_power_dbm,avg_sideband_dbc,avg_sideband_dbc_per_hz,csv_path"
    ]
    for step in sorted(
        phase_steps,
        key=lambda item: (item.expected_freq_hz[0], item.metrics.offset_hz, item.step_index),
    ):
        metrics = step.metrics
        left = next((item for item in metrics.sidebands if item.label == "left"), None)
        right = next((item for item in metrics.sidebands if item.label == "right"), None)
        lines.append(
            f"{step.expected_freq_hz[0] / 1e6:.6f},"
            f"{metrics.offset_hz:.6f},"
            f"{metrics.carrier_power_dbm:.6f},"
            f"{metrics.carrier_freq_hz / 1e6:.6f},"
            f"{'' if left is None or left.power_dbm is None else f'{left.power_dbm:.6f}'},"
            f"{'' if left is None or left.freq_hz is None else f'{left.freq_hz / 1e6:.6f}'},"
            f"{'' if right is None or right.power_dbm is None else f'{right.power_dbm:.6f}'},"
            f"{'' if right is None or right.freq_hz is None else f'{right.freq_hz / 1e6:.6f}'},"
            f"{'' if metrics.avg_sideband_power_dbm is None else f'{metrics.avg_sideband_power_dbm:.6f}'},"
            f"{'' if metrics.avg_sideband_dbc is None else f'{metrics.avg_sideband_dbc:.6f}'},"
            f"{'' if metrics.avg_sideband_dbc_per_hz is None else f'{metrics.avg_sideband_dbc_per_hz:.6f}'},"
            f"{step.csv_path}"
        )

    output_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_dynamic_results_csv(steps: Sequence[StepCaptureSummary], output_path: Path) -> None:
    dynamic_steps = [step for step in steps if step.group == "dynamic_sfdr"]
    if not dynamic_steps:
        return

    lines = [
        "name,dwell_ms,transitions,active_duration_ms,measured_elapsed_us,measured_us_per_transition,"
        "intended1_label,intended1_power_dbm,intended1_freq_mhz,"
        "intended2_label,intended2_power_dbm,intended2_freq_mhz,"
        "reference_power_dbm,reference_freq_mhz,spur_power_dbm,spur_freq_mhz,dynamic_spur_margin_db,csv_path"
    ]
    for step in sorted(dynamic_steps, key=lambda item: item.step_index):
        metrics = step.metrics
        intended = list(metrics.intended_peaks[:2])
        while len(intended) < 2:
            intended.append(WindowPeak("", 0.0, 0.0, None, None))
        lines.append(
            f"{step.name},"
            f"{metrics.dwell_ms},"
            f"{metrics.transitions},"
            f"{metrics.active_duration_ms},"
            f"{'' if metrics.measured_elapsed_us is None else metrics.measured_elapsed_us},"
            f"{'' if metrics.measured_us_per_transition is None else f'{metrics.measured_us_per_transition:.6f}'},"
            f"{intended[0].label},"
            f"{'' if intended[0].power_dbm is None else f'{intended[0].power_dbm:.6f}'},"
            f"{'' if intended[0].freq_hz is None else f'{intended[0].freq_hz / 1e6:.6f}'},"
            f"{intended[1].label},"
            f"{'' if intended[1].power_dbm is None else f'{intended[1].power_dbm:.6f}'},"
            f"{'' if intended[1].freq_hz is None else f'{intended[1].freq_hz / 1e6:.6f}'},"
            f"{'' if metrics.reference_power_dbm is None else f'{metrics.reference_power_dbm:.6f}'},"
            f"{'' if metrics.reference_freq_hz is None else f'{metrics.reference_freq_hz / 1e6:.6f}'},"
            f"{'' if metrics.spur_power_dbm is None else f'{metrics.spur_power_dbm:.6f}'},"
            f"{'' if metrics.spur_freq_hz is None else f'{metrics.spur_freq_hz / 1e6:.6f}'},"
            f"{'' if metrics.dynamic_spur_margin_db is None else f'{metrics.dynamic_spur_margin_db:.6f}'},"
            f"{step.csv_path}"
        )

    output_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def require_prompt(
    seen_prompt: Optional[str],
    required_prompt: str,
    feature_name: str,
) -> None:
    if seen_prompt == required_prompt:
        return

    if seen_prompt is None:
        raise RuntimeError(
            f"{feature_name} was enabled, but the firmware never reached the expected prompt "
            f"'{required_prompt}'."
        )

    raise RuntimeError(
        f"{feature_name} was enabled, but the firmware reached '{seen_prompt}' before "
        f"'{required_prompt}'. The programmed image does not match the expected prompt flow."
    )


def main() -> int:
    args = parse_args()
    ensure_args(args)
    if args.run_scheduler_benchmark_suite:
        return run_scheduler_benchmark_suite_mode(args)
    if args.run_full_integration:
        return run_full_integration_mode(args)
    if awg_scheduler_console_requested(args):
        return run_awg_scheduler_console_mode(args)

    script_dir = Path(__file__).resolve().parent
    project_dir = script_dir
    output_dir = Path(args.output_dir) if args.output_dir else script_dir / "capture_runs" / utc_timestamp()
    output_dir.mkdir(parents=True, exist_ok=True)

    console_log = ConsoleLog(output_dir / "uart.log")
    uart = None
    analyzer = None

    try:
        analyzer = RohdeSchwarzFSH(args.visa_resource, args.visa_backend, args.analyzer_timeout)
        print(f"[HOST] Analyzer connected: {analyzer.idn}")
        if args.analyzer_preset != "off":
            print(f"[HOST] Applying analyzer preset: {args.analyzer_preset}")
            analyzer.apply_preset(args.analyzer_preset)
            print("[HOST] Analyzer preset complete.")

        analyzer_settings = build_analyzer_settings(args)
        sfdr_settings = build_sfdr_settings(args)
        phase_noise_requests = build_phase_noise_requests(args)
        phase_noise_settings = build_phase_noise_settings(args, analyzer_settings)
        phase_noise_offset_requests = build_phase_noise_offset_requests(args, phase_noise_settings)
        dynamic_settings = build_dynamic_settings(args, analyzer_settings)
        dynamic_specs = build_dynamic_specs(args)
        dds_band_step_specs = build_dds_band_step_specs(args)
        sfdr_step_specs = build_sfdr_step_specs(args)
        extra_cflags = build_sweep_override_cflags(args)
        benchmark_prompts_disabled = False

        trace_capture_flags: List[str] = []
        if analyzer_settings.capture_trace:
            trace_capture_flags.append("--capture-trace")
        if phase_noise_requests:
            trace_capture_flags.append("--phase-noise-span-hz")
        if analyzer.legacy_firmware and trace_capture_flags:
            raise SystemExit(
                f"{analyzer.trace_capture_unsupported_message()} "
                f"Requested option(s): {', '.join(trace_capture_flags)}."
            )

        uart = UartCoordinator(
            port=args.serial_port,
            baudrate=args.baudrate,
            timeout_s=args.uart_timeout,
            log=console_log,
            dtr=tristate_arg(args.serial_dtr),
            rts=tristate_arg(args.serial_rts),
        )
        print(f"[HOST] UART connected: {args.serial_port} @ {args.baudrate}")

        settings_files = resolve_xilinx_settings(args)
        if not args.skip_make_run:
            rx_before = uart.rx_count
            for item in settings_files:
                print(f"[HOST] Using Xilinx settings: {item}")
            if extra_cflags:
                print("[HOST] Launching 'make update && make clean && make run' with generated/user NEW_CFLAGS...")
            else:
                print("[HOST] Launching 'make run'...")
            run_make_launch(
                project_dir=project_dir,
                timeout_s=args.make_timeout,
                settings_files=settings_files,
                cflags=extra_cflags,
                refresh_build=bool(extra_cflags),
                uart=uart,
            )
            print("[HOST] 'make run' completed.")

            if uart.rx_count == rx_before:
                print(f"[HOST] Reopening UART {args.serial_port} after programming...")
                uart.reopen()
            else:
                print("[HOST] UART data already active after programming; keeping current serial session.")

        if args.resume_at_nco:
            uart.wait_for(NCO_START_PROMPT, args.uart_timeout)
        else:
            nco_prompt_consumed = advance_boot_defaults_or_wait_for_nco(
                uart=uart,
                clock_reply=args.clock_choice,
                rate_reply=args.rate_choice,
                timeout_s=args.uart_timeout,
            )
            if not nco_prompt_consumed:
                uart.wait_for(NCO_START_PROMPT, args.uart_timeout)

        steps: List[StepCaptureSummary] = []
        throughput_results: List[dict] = []
        uart_rtt_result = None

        run_nco_test = args.run_nco_test and not args.skip_nco_test
        if not run_nco_test:
            uart.send_line("n")
            print("[HOST] NCO discriminator test skipped; DDS-only benchmarking is the default workflow.")
        else:
            uart.send_line("y")
            print("[HOST] NCO discriminator test started.")
            steps.extend(
                capture_step_group(
                    uart=uart,
                    analyzer=analyzer,
                    output_dir=output_dir,
                    step_specs=NCO_STEP_SPECS,
                    done_marker=NCO_DONE_MARKER,
                    timeout_s=args.uart_timeout,
                    settings=analyzer_settings,
                        benchmark_prompts_enabled=not benchmark_prompts_disabled,
                    dump_analyzer_state=args.dump_analyzer_state,
                    write_step_csv=args.write_step_csv,
                    settle_timeout_s=0.0,
                    settle_error_hz=0.0,
                    use_known_tone_capture=False,
                    lock_to_expected=False,
                )
            )

        next_prompt = wait_for_optional_prompt(
            uart,
            DDS_BAND_START_PROMPT,
            args.uart_timeout,
            extra_needles=[
                AWG_SWEEP_START_PROMPT,
                SFDR_START_PROMPT,
                DYNAMIC_SFDR_START_PROMPT,
                THROUGHPUT_START_PROMPT,
                UART_RTT_START_PROMPT,
                DDS_SWEEP_START_MARKER,
            ],
        )
        if next_prompt == AWG_SWEEP_START_PROMPT:
            if args.run_awg_sweep:
                uart.send_line("y")
                print("[HOST] AWG scheduler sweep started.")
            else:
                uart.send_line("n")
                print("[HOST] AWG scheduler sweep skipped by host option.")
            next_prompt = wait_for_optional_prompt(
                uart,
                DDS_BAND_START_PROMPT,
                args.uart_timeout,
                extra_needles=[
                    SFDR_START_PROMPT,
                    DYNAMIC_SFDR_START_PROMPT,
                    THROUGHPUT_START_PROMPT,
                    UART_RTT_START_PROMPT,
                    DDS_SWEEP_START_MARKER,
                ],
            )
        if next_prompt == DDS_BAND_START_PROMPT:
            if args.skip_dds_band_test:
                uart.send_line("n")
                print("[HOST] DDS-band diagnostic skipped by host option.")
            else:
                uart.send_line("y")
                print("[HOST] DDS-band diagnostic started.")
                steps.extend(
                    capture_step_group(
                        uart=uart,
                        analyzer=analyzer,
                        output_dir=output_dir,
                        step_specs=dds_band_step_specs,
                        done_marker=DDS_BAND_DONE_MARKER,
                        timeout_s=args.uart_timeout,
                        settings=analyzer_settings,
                        benchmark_prompts_enabled=not benchmark_prompts_disabled,
                        dump_analyzer_state=args.dump_analyzer_state,
                        write_step_csv=args.write_step_csv,
                        settle_timeout_s=args.freq_settle_timeout_s,
                        settle_error_hz=args.freq_settle_error_hz,
                        use_known_tone_capture=True,
                        lock_to_expected=True,
                    )
                )
            next_prompt = wait_for_optional_prompt(
                uart,
                SFDR_START_PROMPT,
                args.uart_timeout,
                extra_needles=[
                    DYNAMIC_SFDR_START_PROMPT,
                    THROUGHPUT_START_PROMPT,
                    UART_RTT_START_PROMPT,
                    DDS_SWEEP_START_MARKER,
                ],
            )

        if next_prompt == SFDR_START_PROMPT:
            if args.skip_sfdr_test:
                uart.send_line("n")
                print("[HOST] SFDR tone set skipped by host option.")
            else:
                uart.send_line("y")
                print("[HOST] SFDR tone set started.")
                steps.extend(
                    capture_sfdr_group(
                        uart=uart,
                        analyzer=analyzer,
                        output_dir=output_dir,
                        step_specs=sfdr_step_specs,
                        done_marker=SFDR_DONE_MARKER,
                        timeout_s=args.uart_timeout,
                        settings=analyzer_settings,
                        sfdr_settings=sfdr_settings,
                        phase_noise_requests=phase_noise_requests,
                        phase_noise_offset_requests=phase_noise_offset_requests,
                        phase_noise_settings=phase_noise_settings,
                        benchmark_prompts_enabled=not benchmark_prompts_disabled,
                        dump_analyzer_state=args.dump_analyzer_state,
                        write_step_csv=args.write_step_csv,
                        settle_timeout_s=args.freq_settle_timeout_s,
                        settle_error_hz=args.freq_settle_error_hz,
                    )
                )
            next_prompt = wait_for_optional_prompt(
                uart,
                DYNAMIC_SFDR_START_PROMPT,
                args.uart_timeout,
                extra_needles=[
                    THROUGHPUT_START_PROMPT,
                    UART_RTT_START_PROMPT,
                    DDS_SWEEP_START_MARKER,
                ],
            )

        if not args.skip_dynamic_sfdr_test:
            require_prompt(
                seen_prompt=next_prompt,
                required_prompt=DYNAMIC_SFDR_START_PROMPT,
                feature_name="Dynamic retune settling test",
            )

        if next_prompt == DYNAMIC_SFDR_START_PROMPT:
            if args.skip_dynamic_sfdr_test:
                uart.send_line("n")
                print("[HOST] Dynamic retune settling test skipped by host option.")
            else:
                uart.send_line("y")
                print("[HOST] Dynamic retune settling test started.")
                steps.extend(
                    capture_dynamic_sfdr_group(
                        uart=uart,
                        analyzer=analyzer,
                        output_dir=output_dir,
                        step_specs=dynamic_specs,
                        done_marker=DYNAMIC_SFDR_DONE_MARKER,
                        timeout_s=args.uart_timeout,
                        settings=dynamic_settings,
                        sfdr_settings=sfdr_settings,
                        benchmark_prompts_enabled=not benchmark_prompts_disabled,
                        dump_analyzer_state=args.dump_analyzer_state,
                        write_step_csv=args.write_step_csv,
                    )
                )
            next_prompt = wait_for_optional_prompt(
                uart,
                THROUGHPUT_START_PROMPT,
                args.uart_timeout,
                extra_needles=[UART_RTT_START_PROMPT, DDS_SWEEP_START_MARKER],
            )

        if next_prompt == THROUGHPUT_START_PROMPT:
            if args.skip_throughput_test:
                uart.send_line("n")
                print("[HOST] Throughput benchmark skipped by host option.")
            else:
                uart.send_line("y")
                print("[HOST] Throughput benchmark started.")
                uart.wait_for(THROUGHPUT_DONE_MARKER, args.uart_timeout)
                throughput_results = parse_throughput_results_from_log(console_log.file_path)
                if throughput_results:
                    for item in throughput_results:
                        print(
                            "[HOST] throughput_"
                            f"{item['name']}: ops={item['ops']}, "
                            f"total_us={item['total_us']}, "
                            f"ops_per_sec={item['ops_per_sec']}, "
                            f"ns_per_op={item['ns_per_op']}"
                        )
            next_prompt = wait_for_optional_prompt(
                uart,
                UART_RTT_START_PROMPT,
                args.uart_timeout,
                extra_needles=[DDS_SWEEP_START_MARKER],
            )

        if next_prompt == UART_RTT_START_PROMPT:
            if args.skip_uart_rtt:
                uart.send_line("n")
                print("[HOST] UART RTT benchmark skipped by host option.")
            else:
                uart.send_line("y")
                print("[HOST] UART RTT benchmark started.")
                uart_rtt_result = run_uart_rtt_benchmark(
                    uart=uart,
                    timeout_s=args.uart_timeout,
                    samples=args.uart_rtt_samples,
                )

        summary = {
            "timestamp_utc": utc_timestamp(),
            "analyzer_idn": analyzer.idn,
            "analyzer_preset": args.analyzer_preset,
            "serial_port": args.serial_port,
            "rbw_hz": analyzer_settings.rbw_hz,
            "vbw_hz": analyzer_settings.vbw_hz,
            "sweep_count": analyzer_settings.sweep_count,
            "trace_mode": analyzer_settings.trace_mode,
            "detector": analyzer_settings.detector,
            "reference_level_dbm": analyzer_settings.reference_level_dbm,
            "display_range_db": analyzer_settings.display_range_db,
            "attenuation_auto": analyzer_settings.attenuation_auto,
            "preamp_on": analyzer_settings.preamp_on,
            "impedance_ohms": analyzer_settings.impedance_ohms,
            "capture_trace": analyzer_settings.capture_trace,
            "sfdr_settings": asdict(sfdr_settings),
            "dds_band_sweep": {
                "start_hz": dds_band_step_specs[0].expected_freq_hz[0] if dds_band_step_specs else None,
                "stop_hz": dds_band_step_specs[-1].expected_freq_hz[0] if dds_band_step_specs else None,
                "step_count": len(dds_band_step_specs),
                "custom": parse_optional_sweep_range(
                    args.dds_band_sweep_start_hz,
                    args.dds_band_sweep_stop_hz,
                    args.dds_band_sweep_step_hz,
                    "DDS-band",
                ) is not None,
            },
            "sfdr_sweep": {
                "start_hz": sfdr_step_specs[0].expected_freq_hz[0] if sfdr_step_specs else None,
                "stop_hz": sfdr_step_specs[-1].expected_freq_hz[0] if sfdr_step_specs else None,
                "step_count": len(sfdr_step_specs),
                "custom": parse_optional_sweep_range(
                    args.sfdr_sweep_start_hz,
                    args.sfdr_sweep_stop_hz,
                    args.sfdr_sweep_step_hz,
                    "SFDR",
                ) is not None,
            },
            "phase_noise": {
                "enabled": bool(phase_noise_requests) or bool(phase_noise_offset_requests),
                "carriers_mhz": sorted(
                    {request.carrier_hz / 1e6 for request in phase_noise_requests}
                    | {request.carrier_hz / 1e6 for request in phase_noise_offset_requests}
                ),
                "spans_hz": sorted({request.span_hz for request in phase_noise_requests}),
                "settings": asdict(phase_noise_settings),
            },
            "phase_noise_offset": {
                "enabled": bool(phase_noise_offset_requests),
                "carriers_mhz": sorted({request.carrier_hz / 1e6 for request in phase_noise_offset_requests}),
                "offsets_hz": sorted({request.offset_hz for request in phase_noise_offset_requests}),
                "sideband_window_hz": (
                    sorted({request.sideband_window_hz for request in phase_noise_offset_requests})
                    if phase_noise_offset_requests
                    else []
                ),
                "settings": asdict(phase_noise_settings),
            },
            "dynamic_sfdr": {
                "enabled": not args.skip_dynamic_sfdr_test,
                "steps": [asdict(item) for item in dynamic_specs],
                "settings": asdict(dynamic_settings),
            },
            "throughput": throughput_results,
            "uart_rtt": uart_rtt_result,
            "steps": steps,
        }
        if args.dump_analyzer_state:
            summary["final_analyzer_state"] = analyzer.readback_state()
        summary_path = output_dir / "summary.json"
        summary_path.write_text(
            json.dumps(summary, indent=2, default=json_default) + "\n",
            encoding="utf-8",
        )

        if throughput_results:
            (output_dir / "throughput.json").write_text(
                json.dumps(throughput_results, indent=2) + "\n",
                encoding="utf-8",
            )
        if uart_rtt_result is not None:
            (output_dir / "uart_rtt.json").write_text(
                json.dumps(uart_rtt_result, indent=2) + "\n",
                encoding="utf-8",
            )

        write_dds_band_artifacts(summary_path, output_dir, output_dir.name)
        write_sfdr_results_csv(steps, output_dir / "sfdr_results.csv")
        write_phase_noise_results_csv(steps, output_dir / "phase_noise_results.csv")
        write_phase_noise_offset_results_csv(steps, output_dir / "phase_noise_offset_results.csv")
        write_dynamic_results_csv(steps, output_dir / "dynamic_sfdr_results.csv")
        print(f"[HOST] Capture complete. Artifacts written to: {output_dir}")
        return 0
    except Exception as exc:
        print(f"\n[HOST] ERROR: {exc}")
        return 1
    finally:
        if uart is not None:
            try:
                uart.close()
            except Exception:
                pass
        if analyzer is not None:
            try:
                analyzer.close()
            except Exception:
                pass
        console_log.close()


if __name__ == "__main__":
    raise SystemExit(main())
