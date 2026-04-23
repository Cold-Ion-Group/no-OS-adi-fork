#!/usr/bin/env python3
"""
Coordinate the FMCDAC UART diagnostics with R&S FSH8 spectrum-analyzer captures.

The primary workflow is now DDS-centric:
1. Open the DUT UART.
2. Optionally launch `make run` from `projects/fmcdac`.
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
DDS_BAND_START_PROMPT = "[DDS-BAND] Run focused DDS sweep diagnostic around 230-330 MHz? [y/N]:"
DDS_BAND_DONE_MARKER = "[DDS-BAND] Completed focused DDS band diagnostic. Returning to normal DDS sweep."
SFDR_START_PROMPT = "[SFDR-TEST] Run steady-state SFDR tone set at 50-400 MHz? [y/N]:"
SFDR_DONE_MARKER = "[SFDR-TEST] Completed steady-state SFDR tone set."
DYNAMIC_SFDR_START_PROMPT = "[DYNAMIC-SFDR] Run dynamic retune settling test? [y/N]:"
DYNAMIC_SFDR_DONE_MARKER = "[DYNAMIC-SFDR] Completed dynamic retune settling test."
THROUGHPUT_START_PROMPT = "[THROUGHPUT] Run MicroBlaze throughput benchmark? [y/N]:"
THROUGHPUT_DONE_MARKER = "[THROUGHPUT] Done."
UART_RTT_START_PROMPT = "[UART-RTT] Run host UART round-trip benchmark? [y/N]:"
UART_RTT_READY_MARKER = "[UART-RTT] Ready. Send 'PING <token>' and wait for 'PONG <token>'. Send DONE to exit."
UART_RTT_DONE_MARKER = "[UART-RTT] Done."
DDS_SWEEP_START_MARKER = "[DDS] AXI DAC core:"
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

DDS_BAND_STEP_SPECS = [
    StepSpec("dds_band", 1, "dds_10mhz", "[DDS-BAND] Step 1/14: 10 MHz DDS tone.", "DDS reference at 10 MHz", [10_000_000.0], DEFAULT_DDS_SPAN_HZ, DEFAULT_DDS_SEARCH_MARGIN_HZ),
    StepSpec("dds_band", 2, "dds_100mhz", "[DDS-BAND] Step 2/14: 100 MHz DDS tone.", "DDS reference at 100 MHz", [100_000_000.0], DEFAULT_DDS_SPAN_HZ, DEFAULT_DDS_SEARCH_MARGIN_HZ),
    StepSpec("dds_band", 3, "dds_200mhz", "[DDS-BAND] Step 3/14: 200 MHz DDS tone.", "DDS reference at 200 MHz", [200_000_000.0], DEFAULT_DDS_SPAN_HZ, DEFAULT_DDS_SEARCH_MARGIN_HZ),
    StepSpec("dds_band", 4, "dds_230mhz", "[DDS-BAND] Step 4/14: 230 MHz DDS tone.", "Pre-band checkpoint at 230 MHz", [230_000_000.0], DEFAULT_DDS_SPAN_HZ, DEFAULT_DDS_SEARCH_MARGIN_HZ),
    StepSpec("dds_band", 5, "dds_240mhz", "[DDS-BAND] Step 5/14: 240 MHz DDS tone.", "Pre-band checkpoint at 240 MHz", [240_000_000.0], DEFAULT_DDS_SPAN_HZ, DEFAULT_DDS_SEARCH_MARGIN_HZ),
    StepSpec("dds_band", 6, "dds_250mhz", "[DDS-BAND] Step 6/14: 250 MHz DDS tone.", "Approaching the reported droop region", [250_000_000.0], DEFAULT_DDS_SPAN_HZ, DEFAULT_DDS_SEARCH_MARGIN_HZ),
    StepSpec("dds_band", 7, "dds_260mhz", "[DDS-BAND] Step 7/14: 260 MHz DDS tone.", "Start of the reported low-amplitude region", [260_000_000.0], DEFAULT_DDS_SPAN_HZ, DEFAULT_DDS_SEARCH_MARGIN_HZ),
    StepSpec("dds_band", 8, "dds_270mhz", "[DDS-BAND] Step 8/14: 270 MHz DDS tone.", "Problem-band checkpoint at 270 MHz", [270_000_000.0], DEFAULT_DDS_SPAN_HZ, DEFAULT_DDS_SEARCH_MARGIN_HZ),
    StepSpec("dds_band", 9, "dds_280mhz", "[DDS-BAND] Step 9/14: 280 MHz DDS tone.", "Problem-band checkpoint at 280 MHz", [280_000_000.0], DEFAULT_DDS_SPAN_HZ, DEFAULT_DDS_SEARCH_MARGIN_HZ),
    StepSpec("dds_band", 10, "dds_290mhz", "[DDS-BAND] Step 10/14: 290 MHz DDS tone.", "Reported transition to no visible waveform", [290_000_000.0], DEFAULT_DDS_SPAN_HZ, DEFAULT_DDS_SEARCH_MARGIN_HZ),
    StepSpec("dds_band", 11, "dds_300mhz", "[DDS-BAND] Step 11/14: 300 MHz DDS tone.", "Problem-band checkpoint at 300 MHz", [300_000_000.0], DEFAULT_DDS_SPAN_HZ, DEFAULT_DDS_SEARCH_MARGIN_HZ),
    StepSpec("dds_band", 12, "dds_310mhz", "[DDS-BAND] Step 12/14: 310 MHz DDS tone.", "Problem-band checkpoint at 310 MHz", [310_000_000.0], DEFAULT_DDS_SPAN_HZ, DEFAULT_DDS_SEARCH_MARGIN_HZ),
    StepSpec("dds_band", 13, "dds_320mhz", "[DDS-BAND] Step 13/14: 320 MHz DDS tone.", "Problem-band checkpoint at 320 MHz", [320_000_000.0], DEFAULT_DDS_SPAN_HZ, DEFAULT_DDS_SEARCH_MARGIN_HZ),
    StepSpec("dds_band", 14, "dds_330mhz", "[DDS-BAND] Step 14/14: 330 MHz DDS tone.", "Problem-band checkpoint at 330 MHz", [330_000_000.0], DEFAULT_DDS_SPAN_HZ, DEFAULT_DDS_SEARCH_MARGIN_HZ),
]

SFDR_STEP_SPECS = [
    StepSpec("sfdr", 1, "sfdr_50mhz", "[SFDR-TEST] Step 1/8: 50 MHz DDS tone.", "Steady-state SFDR carrier at 50 MHz", [50_000_000.0], DEFAULT_DDS_SPAN_HZ, DEFAULT_DDS_SEARCH_MARGIN_HZ),
    StepSpec("sfdr", 2, "sfdr_100mhz", "[SFDR-TEST] Step 2/8: 100 MHz DDS tone.", "Steady-state SFDR carrier at 100 MHz", [100_000_000.0], DEFAULT_DDS_SPAN_HZ, DEFAULT_DDS_SEARCH_MARGIN_HZ),
    StepSpec("sfdr", 3, "sfdr_150mhz", "[SFDR-TEST] Step 3/8: 150 MHz DDS tone.", "Steady-state SFDR carrier at 150 MHz", [150_000_000.0], DEFAULT_DDS_SPAN_HZ, DEFAULT_DDS_SEARCH_MARGIN_HZ),
    StepSpec("sfdr", 4, "sfdr_200mhz", "[SFDR-TEST] Step 4/8: 200 MHz DDS tone.", "Steady-state SFDR carrier at 200 MHz", [200_000_000.0], DEFAULT_DDS_SPAN_HZ, DEFAULT_DDS_SEARCH_MARGIN_HZ),
    StepSpec("sfdr", 5, "sfdr_250mhz", "[SFDR-TEST] Step 5/8: 250 MHz DDS tone.", "Steady-state SFDR carrier at 250 MHz", [250_000_000.0], DEFAULT_DDS_SPAN_HZ, DEFAULT_DDS_SEARCH_MARGIN_HZ),
    StepSpec("sfdr", 6, "sfdr_300mhz", "[SFDR-TEST] Step 6/8: 300 MHz DDS tone.", "Steady-state SFDR carrier at 300 MHz", [300_000_000.0], DEFAULT_DDS_SPAN_HZ, DEFAULT_DDS_SEARCH_MARGIN_HZ),
    StepSpec("sfdr", 7, "sfdr_350mhz", "[SFDR-TEST] Step 7/8: 350 MHz DDS tone.", "Steady-state SFDR carrier at 350 MHz", [350_000_000.0], DEFAULT_DDS_SPAN_HZ, DEFAULT_DDS_SEARCH_MARGIN_HZ),
    StepSpec("sfdr", 8, "sfdr_400mhz", "[SFDR-TEST] Step 8/8: 400 MHz DDS tone.", "Steady-state SFDR carrier at 400 MHz", [400_000_000.0], DEFAULT_DDS_SPAN_HZ, DEFAULT_DDS_SEARCH_MARGIN_HZ),
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
                time.sleep(0.05)

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


def capture_trace_step(
    analyzer: RohdeSchwarzFSH,
    output_dir: Path,
    step: StepSpec,
    settings: AnalyzerSettings,
) -> StepCaptureSummary:
    trace_freqs_hz, trace_levels_dbm, metrics = analyzer.capture_trace(step, settings)

    csv_path = output_dir / f"step{step.index:02d}_{step.name}.csv"
    json_path = output_dir / f"step{step.index:02d}_{step.name}.json"
    save_trace_csv(csv_path, trace_freqs_hz, trace_levels_dbm)
    write_step_json(json_path, analyzer.idn, step, metrics)

    summary = StepCaptureSummary(
        group=step.group,
        step_index=step.index,
        name=step.name,
        marker=step.marker,
        description=step.description,
        expected_freq_hz=step.expected_freq_hz,
        csv_path=str(csv_path.resolve()),
        metrics=metrics,
    )
    return summary


def write_step_json(
    path: Path,
    analyzer_idn: str,
    step: StepSpec,
    metrics: SpectrumMetrics,
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
        help="Assume the board is already running and do not launch 'make run' first.",
    )
    parser.add_argument(
        "--make-args",
        default="",
        help="Optional extra arguments appended to 'make run'.",
    )
    parser.add_argument(
        "--xilinx-settings",
        action="append",
        default=[],
        help="Optional settings64.bat path(s) to call before 'make run'. Repeat as needed.",
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
        "--make-timeout",
        type=float,
        default=300.0,
        help="Timeout for 'make run' in seconds",
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


def ensure_args(args: argparse.Namespace) -> None:
    if args.list_visa:
        resources = RohdeSchwarzFSH.list_resources(args.visa_backend)
        if resources:
            for item in resources:
                print(item)
        else:
            print("No VISA resources found.")
        raise SystemExit(0)

    if not args.visa_resource:
        raise SystemExit("--visa-resource is required unless --list-visa is used")
    if not args.serial_port:
        raise SystemExit("--serial-port is required for coordinated UART + analyzer operation")
    if args.sweep_count < 1:
        raise SystemExit("--sweep-count must be at least 1")
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


def build_dynamic_specs(args: argparse.Namespace) -> List[DynamicRetuneSpec]:
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

        time.sleep(0.25)

    raise TimeoutError(f"Timed out waiting for UART text: {NCO_START_PROMPT!r}")


def resolve_xilinx_settings(args: argparse.Namespace) -> List[Path]:
    return [Path(item) for item in args.xilinx_settings]


def build_make_run_command(settings_files: List[Path], make_args: str) -> str:
    parts: List[str] = []
    for item in settings_files:
        if not item.is_file():
            raise RuntimeError(f"Xilinx settings file not found: {item}")
        parts.append(f'call "{item}"')

    make_command = "make run"
    if make_args.strip():
        make_command += f" {make_args.strip()}"
    parts.append(make_command)
    return " && ".join(parts)


def run_make_run(
    project_dir: Path,
    uart: UartCoordinator,
    timeout_s: float,
    settings_files: List[Path],
    make_args: str,
) -> None:
    command = build_make_run_command(settings_files, make_args)
    escaped_command = command.replace('"', '""')
    command_line = f'cmd.exe /d /c "{escaped_command}"'
    try:
        proc = subprocess.Popen(
            command_line,
            cwd=str(project_dir),
        )
    except FileNotFoundError as exc:  # pragma: no cover - Windows-specific
        raise RuntimeError("Could not find 'cmd.exe' or 'make' in PATH") from exc

    deadline = time.monotonic() + timeout_s
    while True:
        uart.pump()
        ret = proc.poll()
        if ret is not None:
            while uart.pump():
                pass
            if ret != 0:
                raise RuntimeError(f"'make run' failed with exit code {ret}")
            return
        if time.monotonic() >= deadline:
            proc.kill()
            raise TimeoutError(f"'make run' exceeded timeout of {timeout_s:.0f} seconds")
        time.sleep(0.05)


def capture_step_group(
    uart: UartCoordinator,
    analyzer: RohdeSchwarzFSH,
    output_dir: Path,
    step_specs: Sequence[StepSpec],
    done_marker: str,
    timeout_s: float,
    settings: AnalyzerSettings,
) -> List[StepCaptureSummary]:
    summaries: List[StepCaptureSummary] = []
    reference_power_dbm: Optional[float] = None
    reference_step_name: Optional[str] = None

    for step in step_specs:
        uart.wait_for(step.marker, timeout_s)
        uart.wait_for(CONTINUE_PROMPT, timeout_s)

        summary = capture_trace_step(
            analyzer=analyzer,
            output_dir=output_dir,
            step=step,
            settings=settings,
        )
        metrics = summary.metrics

        if reference_power_dbm is None:
            reference_power_dbm = metrics.power_dbm
            reference_step_name = step.name

        metrics.reference_power_dbm = reference_power_dbm
        metrics.reference_step_name = reference_step_name
        metrics.power_delta_db = metrics.power_dbm - reference_power_dbm
        write_step_json(
            output_dir / f"step{step.index:02d}_{step.name}.json",
            analyzer.idn,
            step,
            metrics,
        )
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
) -> List[StepCaptureSummary]:
    summaries: List[StepCaptureSummary] = []

    for step in step_specs:
        uart.wait_for(step.marker, timeout_s)
        uart.wait_for(CONTINUE_PROMPT, timeout_s)

        metrics = analyzer.capture_sfdr(step, settings, sfdr_settings)
        json_path = output_dir / f"step{step.index:02d}_{step.name}.json"
        csv_path = output_dir / f"step{step.index:02d}_{step.name}.csv"
        csv_path.write_text(
            "marker_type,frequency_hz,level_dbm\n"
            f"carrier,{metrics.power_freq_hz:.6f},{metrics.power_dbm:.6f}\n"
            f"left_spur,{'' if metrics.left_spur_freq_hz is None else f'{metrics.left_spur_freq_hz:.6f}'},{'' if metrics.left_spur_power_dbm is None else f'{metrics.left_spur_power_dbm:.6f}'}\n"
            f"right_spur,{'' if metrics.right_spur_freq_hz is None else f'{metrics.right_spur_freq_hz:.6f}'},{'' if metrics.right_spur_power_dbm is None else f'{metrics.right_spur_power_dbm:.6f}'}\n"
            f"worst_spur,{'' if metrics.spur_freq_hz is None else f'{metrics.spur_freq_hz:.6f}'},{'' if metrics.spur_power_dbm is None else f'{metrics.spur_power_dbm:.6f}'}\n",
            encoding="utf-8",
        )
        write_step_json(
            json_path,
            analyzer.idn,
            step,
            metrics,
            extra={"sfdr_settings": asdict(sfdr_settings)},
        )

        summary = StepCaptureSummary(
            group=step.group,
            step_index=step.index,
            name=step.name,
            marker=step.marker,
            description=step.description,
            expected_freq_hz=step.expected_freq_hz,
            csv_path=str(csv_path.resolve()),
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
    csv_path.write_text("\n".join(lines) + "\n", encoding="utf-8")

    json_path.write_text(
        json.dumps(
            {
                "group": "phase_noise_offset",
                "step_index": step.index,
                "name": step.name,
                "description": step.description,
                "expected_freq_hz": step.expected_freq_hz,
                "metrics": asdict(metrics),
                "analyzer_idn": analyzer.idn,
            },
            indent=2,
            default=json_default,
        )
        + "\n",
        encoding="utf-8",
    )

    return StepCaptureSummary(
        group="phase_noise_offset",
        step_index=step.index,
        name=step.name,
        marker=step.marker,
        description=step.description,
        expected_freq_hz=step.expected_freq_hz,
        csv_path=str(csv_path.resolve()),
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
) -> DynamicRetuneMetrics:
    exclusion_guard_hz = max(sfdr_settings.carrier_guard_hz, step.intended_margin_hz)
    intended_peaks: List[WindowPeak] = []
    for freq_hz in step.intended_freq_hz:
        left_hz = max(sfdr_settings.search_start_hz, freq_hz - step.intended_margin_hz)
        right_hz = min(sfdr_settings.search_stop_hz, freq_hz + step.intended_margin_hz)
        center_hz = (left_hz + right_hz) / 2.0
        span_hz = max(right_hz - left_hz, 1.0)
        power_dbm, peak_freq_hz = analyzer._capture_peak_for_span(center_hz, span_hz, settings)
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

    print(
        f"[HOST] {step.name}: dwell_ms={metrics.dwell_ms}, "
        f"transitions={metrics.transitions}, "
        f"active_ms~={metrics.active_duration_ms}, "
        + ", ".join(intended_parts)
        + f", {spur_text}, {margin_text}"
    )


def capture_dynamic_sfdr_group(
    uart: UartCoordinator,
    analyzer: RohdeSchwarzFSH,
    output_dir: Path,
    step_specs: Sequence[DynamicRetuneSpec],
    done_marker: str,
    timeout_s: float,
    settings: AnalyzerSettings,
    sfdr_settings: SfdrSettings,
) -> List[StepCaptureSummary]:
    summaries: List[StepCaptureSummary] = []

    for step in step_specs:
        uart.wait_for(step.marker, timeout_s)
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
        csv_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        json_path.write_text(
            json.dumps(
                {
                    "group": "dynamic_sfdr",
                    "step_index": step.index,
                    "name": step.name,
                    "marker": step.marker,
                    "description": step.description,
                    "intended_freq_hz": step.intended_freq_hz,
                    "metrics": asdict(metrics),
                    "sfdr_settings": asdict(sfdr_settings),
                    "analyzer_idn": analyzer.idn,
                },
                indent=2,
                default=json_default,
            )
            + "\n",
            encoding="utf-8",
        )

        summary = StepCaptureSummary(
            group="dynamic_sfdr",
            step_index=step.index,
            name=step.name,
            marker=step.marker,
            description=step.description,
            expected_freq_hz=list(step.intended_freq_hz),
            csv_path=str(csv_path.resolve()),
            metrics=metrics,
        )
        summaries.append(summary)
        print_dynamic_summary(step, metrics)
        uart.wait_for(step.done_marker, timeout_s)

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
    try:
        series = load_dds_band_series(summary_path, label)
    except Exception:
        return

    if not series.points:
        return

    write_dds_band_csv([series], output_dir / "dds_band_plot.csv")
    write_dds_band_svg(
        [series],
        output_dir / "dds_band_plot.svg",
        "DDS Band Level Delta vs Frequency",
    )


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
        "name,dwell_ms,transitions,active_duration_ms,reference_power_dbm,reference_freq_mhz,spur_power_dbm,spur_freq_mhz,dynamic_spur_margin_db,csv_path"
    ]
    for step in sorted(dynamic_steps, key=lambda item: item.step_index):
        metrics = step.metrics
        lines.append(
            f"{step.name},"
            f"{metrics.dwell_ms},"
            f"{metrics.transitions},"
            f"{metrics.active_duration_ms},"
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

        analyzer_settings = build_analyzer_settings(args)
        sfdr_settings = build_sfdr_settings(args)
        phase_noise_requests = build_phase_noise_requests(args)
        phase_noise_settings = build_phase_noise_settings(args, analyzer_settings)
        phase_noise_offset_requests = build_phase_noise_offset_requests(args, phase_noise_settings)
        dynamic_settings = build_dynamic_settings(args, analyzer_settings)
        dynamic_specs = build_dynamic_specs(args)

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
            print("[HOST] Launching 'make run'...")
            run_make_run(
                project_dir=project_dir,
                uart=uart,
                timeout_s=args.make_timeout,
                settings_files=settings_files,
                make_args=args.make_args,
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
                )
            )

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
                        step_specs=DDS_BAND_STEP_SPECS,
                        done_marker=DDS_BAND_DONE_MARKER,
                        timeout_s=args.uart_timeout,
                        settings=analyzer_settings,
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
                        step_specs=SFDR_STEP_SPECS,
                        done_marker=SFDR_DONE_MARKER,
                        timeout_s=args.uart_timeout,
                        settings=analyzer_settings,
                        sfdr_settings=sfdr_settings,
                        phase_noise_requests=phase_noise_requests,
                        phase_noise_offset_requests=phase_noise_offset_requests,
                        phase_noise_settings=phase_noise_settings,
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
