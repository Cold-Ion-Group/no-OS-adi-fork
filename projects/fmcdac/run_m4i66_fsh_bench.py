#!/usr/bin/env python3
"""
Benchmark a Spectrum M4i.66xx AWG against the same FSH-based analog checks used
for FMCDAC, plus M4i-specific control-path and memory-stress benchmarks.

This script is intentionally separate from the UART-driven FMCDAC flow:

1. DDS-band analog sweep on the R&S FSH
2. Steady-state SFDR sweep on the R&S FSH
3. Host-driven dynamic retune bursts measured on the R&S FSH
4. DDS update latency / update-rate benchmarks on the M4i control path
5. Dense DDS queue-pressure benchmarks
6. Replay-memory upload bandwidth / capacity probes
7. DDS and replay-memory pulse-switching limits

Measurement scope note:
- The update-benchmark latency figures are host-to-driver/card commit timings,
  not direct analog output propagation latencies.
- Pulse-switching can be validated on an MSO22 without connecting the FSH; the
  analog FSH path is only required for the DDS-band, SFDR, and dynamic-SFDR
  sections.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
import threading
import time
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable, List, Optional, Sequence

from run_nco_scope_test import (
    AnalyzerSettings,
    DynamicRetuneSpec,
    RohdeSchwarzFSH,
    SfdrSettings,
    StepCaptureSummary,
    StepSpec,
    build_single_tone_step_specs,
    build_step_extra,
    build_uniform_freq_list,
    capture_dynamic_retune_metrics,
    capture_trace_step,
    json_default,
    print_dynamic_summary,
    print_step_summary,
    write_dynamic_results_csv,
    write_sfdr_results_csv,
    write_step_json,
)

try:
    import numpy as np
except ImportError:  # pragma: no cover - environment-dependent
    np = None

try:
    import spcm
except ImportError:  # pragma: no cover - environment-dependent
    spcm = None

try:
    import pyvisa
except ImportError:  # pragma: no cover - environment-dependent
    pyvisa = None


DEFAULT_DDS_BAND_FREQS_HZ = [
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
DEFAULT_SFDR_FREQS_HZ = [
    50_000_000.0,
    100_000_000.0,
    150_000_000.0,
    200_000_000.0,
    250_000_000.0,
    300_000_000.0,
    350_000_000.0,
    400_000_000.0,
]
DEFAULT_DYNAMIC_STEPS = [
    DynamicRetuneSpec(
        index=1,
        name="m4i66_dynamic_toggle_100_400_1ms",
        marker="[M4I66-DYNAMIC] Step 1/2: toggle_100_to_400_1ms.",
        done_marker="[M4I66-DYNAMIC] Completed burst 1/2.",
        description="Host-driven DDS retune burst toggling 100 MHz <-> 400 MHz with 1 ms dwell",
        intended_freq_hz=[100_000_000.0, 400_000_000.0],
        dwell_ms=1,
        transitions=12_000,
        intended_margin_hz=10_000_000.0,
    ),
    DynamicRetuneSpec(
        index=2,
        name="m4i66_dynamic_toggle_100_400_10ms",
        marker="[M4I66-DYNAMIC] Step 2/2: toggle_100_to_400_10ms.",
        done_marker="[M4I66-DYNAMIC] Completed burst 2/2.",
        description="Host-driven DDS retune burst toggling 100 MHz <-> 400 MHz with 10 ms dwell",
        intended_freq_hz=[100_000_000.0, 400_000_000.0],
        dwell_ms=10,
        transitions=1_200,
        intended_margin_hz=10_000_000.0,
    ),
]


@dataclass
class M4iCardInfo:
    identifier: str
    product_name: Optional[str]
    serial_number: Optional[int]
    active_channel_mask: int
    active_channel_count: int
    bits_per_sample: Optional[int]
    bytes_per_sample: Optional[int]
    max_sample_value: Optional[int]
    sample_rate_hz: Optional[int]
    onboard_memory_samples: Optional[int]
    dds_queue_capacity: Optional[int]
    dds_available: bool


@dataclass
class UpdateBenchmarkResult:
    name: str
    active_cores: int
    updates: int
    commands_per_update: int
    exec_mode: str
    requested_interval_us: Optional[float]
    avg_latency_us: float
    min_latency_us: float
    p50_latency_us: float
    p95_latency_us: float
    max_latency_us: float
    achieved_update_rate_hz: float
    achieved_command_rate_hz: float
    avg_interval_us: Optional[float]
    min_interval_us: Optional[float]
    p95_interval_us: Optional[float]
    max_interval_us: Optional[float]
    queue_high_water: Optional[int]
    notes: List[str]


@dataclass
class QueueProbeResult:
    name: str
    active_cores: int
    queued_updates: int
    commands_per_update: int
    queue_capacity: Optional[int]
    queue_high_water: Optional[int]
    fill_elapsed_s: float
    avg_update_enqueue_us: float
    achieved_command_rate_hz: float
    stopped_reason: str


@dataclass
class MemoryBenchmarkResult:
    name: str
    samples_per_channel: int
    active_channels: int
    transfers: int
    bytes_per_sample: int
    bytes_transferred: int
    elapsed_s: float
    bandwidth_bytes_per_s: float
    avg_transfer_latency_us: float
    min_transfer_latency_us: float
    p95_transfer_latency_us: float
    max_transfer_latency_us: float
    notes: List[str]


@dataclass
class PulseSwitchBenchmarkResult:
    name: str
    mode: str
    status: str
    carrier_hz: Optional[float]
    requested_frequency_hz: Optional[float]
    achieved_pulse_cycle_frequency_hz: Optional[float]
    achieved_edge_update_rate_hz: Optional[float]
    max_replay_pulse_cycle_frequency_hz: Optional[float]
    replay_pulse_cycle_frequency_hz: Optional[float]
    sample_rate_hz: Optional[float]
    period_samples: Optional[int]
    high_samples: Optional[int]
    low_samples: Optional[int]
    duty_cycle_percent: Optional[float]
    active_channels: int
    onboard_memory_samples: Optional[int]
    dds_queue_capacity: Optional[int]
    queued_pulse_cycles_fit: Optional[int]
    queued_duration_at_achieved_s: Optional[float]
    cycles_fit_in_memory: Optional[int]
    memory_duration_s: Optional[float]
    target_duration_s: Optional[float]
    samples_required_for_target_duration: Optional[int]
    memory_limited_for_target_duration: Optional[bool]
    continuous_loop_memory_limited: bool
    upload_samples_per_channel: Optional[int]
    upload_bytes: Optional[int]
    upload_elapsed_s: Optional[float]
    upload_bandwidth_bytes_per_s: Optional[float]
    avg_toggle_latency_us: Optional[float]
    min_toggle_latency_us: Optional[float]
    p50_toggle_latency_us: Optional[float]
    p95_toggle_latency_us: Optional[float]
    max_toggle_latency_us: Optional[float]
    scope_capture: Optional[dict]
    error: Optional[str]
    notes: List[str]


def require_dependency(module, package_name: str) -> None:
    if module is None:
        raise SystemExit(
            f"Missing dependency: {package_name}. "
            f"Install it first, for example: python -m pip install {package_name}"
        )


def utc_timestamp() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def percentile(values: Sequence[float], q: float) -> float:
    if not values:
        raise ValueError("No values supplied")
    if len(values) == 1:
        return values[0]
    return statistics.quantiles(values, n=100, method="inclusive")[max(0, min(99, int(q) - 1))]


def count_bits(mask: int) -> int:
    value = mask
    count = 0
    while value:
        count += value & 1
        value >>= 1
    return count


def call_first(
    obj: Any,
    candidates: Sequence[str],
    *args,
    optional: bool = False,
    **kwargs,
) -> Any:
    for name in candidates:
        method = getattr(obj, name, None)
        if callable(method):
            return method(*args, **kwargs)
    if optional:
        return None
    raise RuntimeError(
        f"{type(obj).__name__} does not provide any of: {', '.join(candidates)}"
    )


def safe_call_first(
    obj: Any,
    candidates: Sequence[str],
    *args,
    **kwargs,
) -> Any:
    try:
        return call_first(obj, candidates, *args, optional=True, **kwargs)
    except Exception:
        return None


def get_spcm_constant(name: str, optional: bool = False) -> Optional[int]:
    require_dependency(spcm, "spcm")
    if hasattr(spcm, name):
        return getattr(spcm, name)
    if optional:
        return None
    raise RuntimeError(f"spcm is missing constant '{name}'")


class TektronixMSO22:
    def __init__(
        self,
        resource_name: str,
        visa_backend: Optional[str],
        timeout_s: float,
    ):
        require_dependency(pyvisa, "pyvisa pyvisa-py")
        self.visa_backend = visa_backend
        self.rm = pyvisa.ResourceManager(visa_backend) if visa_backend else pyvisa.ResourceManager()
        self.inst = self.rm.open_resource(resource_name)
        self.inst.timeout = int(timeout_s * 1000)
        self.inst.chunk_size = 1024 * 1024
        self.inst.write_termination = "\n"
        self.inst.read_termination = "\n"
        self.last_io = "*IDN?"
        self.idn = self._query_text("*IDN?").strip()

    def close(self) -> None:
        try:
            if self.inst is not None:
                self.inst.close()
        finally:
            if self.rm is not None:
                self.rm.close()

    def _wrap_io_error(self, exc: Exception) -> RuntimeError:
        message = f"Scope SCPI failed at '{self.last_io}': {exc}"
        if self.visa_backend != "@py" and (
            "VI_ERROR_RSRC_LOCKED" in str(exc) or "-1073807345" in str(exc)
        ):
            message += ' Try --scope-visa-backend "@py" for MSO22 TCP/IP access.'
        return RuntimeError(message)

    def _write(self, command: str) -> None:
        self.last_io = command
        try:
            self.inst.write(command)
        except Exception as exc:
            raise self._wrap_io_error(exc) from exc

    def _query_text(self, command: str) -> str:
        self.last_io = command
        try:
            return self.inst.query(command).strip()
        except Exception as exc:
            raise self._wrap_io_error(exc) from exc

    def _query_float(self, command: str, default: Optional[float] = None) -> Optional[float]:
        try:
            return float(self._query_text(command))
        except Exception:
            return default

    def _safe_write(self, command: str) -> None:
        try:
            self._write(command)
        except Exception:
            pass

    def capture_waveform(
        self,
        channel: str,
        capture_s: float,
        expected_frequency_hz: Optional[float],
        vertical_scale_v: float,
    ) -> tuple[List[float], List[float], dict]:
        channel = channel.upper()
        capture_s = max(0.01, float(capture_s))

        self._safe_write("HEADER OFF")
        self._safe_write("VERBOSE OFF")
        self._safe_write(f"SELECT:{channel} ON")
        self._safe_write(f"{channel}:COUPLING DC")
        self._safe_write(f"{channel}:SCALE {vertical_scale_v}")
        self._safe_write("ACQUIRE:STATE STOP")
        self._safe_write("ACQUIRE:MODE SAMPLE")
        self._safe_write("ACQUIRE:STOPAFTER RUNSTOP")
        horizontal_scale = capture_s / 10.0
        self._safe_write(f"HORIZONTAL:SCALE {horizontal_scale}")
        if expected_frequency_hz and expected_frequency_hz > 0.0:
            self._safe_write(f"TRIGGER:A:EDGE:SOURCE {channel}")
            self._safe_write("TRIGGER:A:TYPE EDGE")
            self._safe_write("TRIGGER:A:EDGE:SLOPE RISE")
            self._safe_write("TRIGGER:A:LEVEL 0")
        self._safe_write("ACQUIRE:STATE RUN")
        time.sleep(capture_s)
        self._safe_write("ACQUIRE:STATE STOP")

        self._safe_write(f"DATA:SOURCE {channel}")
        record_length = int(self._query_float("HORIZONTAL:RECORDLENGTH?", 10000) or 10000)
        record_length = max(100, min(record_length, 5_000_000))
        self._safe_write("DATA:START 1")
        self._safe_write(f"DATA:STOP {record_length}")
        self._safe_write("DATA:WIDTH 1")
        self._safe_write("DATA:ENCDG RIBINARY")

        x_increment = float(self._query_float("WFMOUTPRE:XINCR?", 1.0) or 1.0)
        x_zero = float(self._query_float("WFMOUTPRE:XZERO?", 0.0) or 0.0)
        y_multiplier = float(self._query_float("WFMOUTPRE:YMULT?", 1.0) or 1.0)
        y_offset = float(self._query_float("WFMOUTPRE:YOFF?", 0.0) or 0.0)
        y_zero = float(self._query_float("WFMOUTPRE:YZERO?", 0.0) or 0.0)

        previous_read_termination = self.inst.read_termination
        self.last_io = "CURVE?"
        try:
            self.inst.read_termination = None
            raw_values = self.inst.query_binary_values(
                "CURVE?",
                datatype="b",
                is_big_endian=True,
                container=list,
                header_fmt="ieee",
                expect_termination=False,
            )
        except Exception as exc:
            raise self._wrap_io_error(exc) from exc
        finally:
            self.inst.read_termination = previous_read_termination

        times_s = [x_zero + (idx * x_increment) for idx in range(len(raw_values))]
        volts = [((float(value) - y_offset) * y_multiplier) + y_zero for value in raw_values]
        metadata = {
            "scope_idn": self.idn,
            "channel": channel,
            "sample_count": len(raw_values),
            "x_increment_s": x_increment,
            "x_zero_s": x_zero,
            "y_multiplier": y_multiplier,
            "y_offset": y_offset,
            "y_zero": y_zero,
            "capture_s": capture_s,
        }
        return times_s, volts, metadata


class SpectrumM4i66Controller:
    def __init__(self, args: argparse.Namespace):
        require_dependency(spcm, "spcm")
        self.args = args
        self.card = self._open_card(args.card_identifier)
        self.channels = None
        self.clock = None
        self.trigger = None
        self.dds = None
        self.active_channel_mask = args.channel_mask
        self.active_channel_count = count_bits(args.channel_mask)
        self.bytes_per_sample = args.assume_bytes_per_sample
        self._started = False
        self._dds_setup_done = False
        self._configure_card()

    def _open_card(self, identifier: str):
        if identifier:
            try:
                card = spcm.Card(identifier)
            except TypeError:
                card = spcm.Card(device_identifier=identifier)
        else:
            try:
                card = spcm.Card(card_type=get_spcm_constant("SPCM_TYPE_AO"))
            except TypeError:
                card = spcm.Card()
        if getattr(card, "_closed", False):
            opened_card = call_first(card, ("open",), optional=True)
            if opened_card is not None:
                card = opened_card
        return card

    def close(self) -> None:
        try:
            if self.card is not None:
                call_first(self.card, ("stop",), optional=True)
        finally:
            call_first(self.card, ("close",), optional=True)

    def _configure_card(self) -> None:
        call_first(self.card, ("reset",), optional=True)
        call_first(self.card, ("timeout", "set_timeout"), self.args.card_timeout_ms, optional=True)

        self.channels = spcm.Channels(self.card, card_enable=self.active_channel_mask)
        for channel in self.channels:
            call_first(channel, ("enable",), True, optional=True)
            call_first(channel, ("amp", "amplitude"), self.args.channel_amplitude_mv, optional=True)
            if self.args.output_load_ohms > 0:
                call_first(
                    channel,
                    ("output_load", "load"),
                    self.args.output_load_ohms,
                    optional=True,
                )

        self.clock = spcm.Clock(self.card)
        if self.args.reference_clock_hz:
            call_first(self.clock, ("reference_clock",), self.args.reference_clock_hz, optional=True)
        clock_mode = get_spcm_constant("SPC_CM_INTPLL", optional=True)
        if clock_mode is not None:
            call_first(self.clock, ("mode",), clock_mode, optional=True)
        call_first(
            self.clock,
            ("sample_rate", "samplerate"),
            int(self.args.sample_rate_hz),
            optional=True,
        )

        self.trigger = spcm.Trigger(self.card)
        software_mask = get_spcm_constant("SPC_TMASK_SOFTWARE", optional=True)
        if software_mask is not None:
            call_first(self.trigger, ("or_mask", "ormask"), software_mask, optional=True)
            call_first(self.trigger, ("write_setup",), optional=True)

        bits_per_sample = safe_call_first(self.card, ("bits_per_sample",))
        if bits_per_sample is not None:
            self.bytes_per_sample = max(1, (int(bits_per_sample) + 7) // 8)
        bytes_per_sample = safe_call_first(self.card, ("bytes_per_sample",))
        if bytes_per_sample is not None:
            self.bytes_per_sample = int(bytes_per_sample)

    def _set_card_mode_constant(self, constant_name: str) -> None:
        mode = get_spcm_constant(constant_name, optional=True)
        if mode is None:
            return
        current_mode = safe_call_first(self.card, ("card_mode",))
        if current_mode == mode:
            return
        if self._started:
            call_first(self.card, ("stop",), optional=True)
            self._started = False
        call_first(self.card, ("card_mode",), mode, optional=False)

    def start_output(self) -> None:
        if self._started:
            return
        call_first(self.card, ("loops",), 0, optional=True)
        enable_cmd = get_spcm_constant("M2CMD_CARD_ENABLETRIGGER", optional=True)
        if enable_cmd is not None:
            call_first(self.card, ("start",), enable_cmd, optional=True)
            call_first(self.trigger, ("force", "force_trigger"), optional=True)
        else:
            call_first(self.card, ("start",), optional=True)
        self._started = True

    def ensure_dds(self) -> None:
        if self.dds is not None:
            self._set_card_mode_constant("SPC_REP_STD_DDS")
            return
        if not self.dds_mode_available():
            raise RuntimeError(
                "This card/firmware does not report SPC_REP_STD_DDS in SPC_AVAILCARDMODES; "
                "DDS command-mode benchmarks are unavailable. Use replay pulse mode instead."
            )
        self._set_card_mode_constant("SPC_REP_STD_DDS")
        try:
            self.dds = spcm.DDS(self.card, channels=self.channels, no_units=True)
        except Exception as exc:
            raise RuntimeError(
                "Could not open the DDS control path on this card. "
                "Install the DDS option / matching spcm package, or skip the DDS benchmarks."
            ) from exc
        call_first(self.dds, ("reset", "clear"), optional=True)
        dds_transfer_single = get_spcm_constant("SPCM_DDS_DTM_SINGLE", optional=True)
        if dds_transfer_single is not None:
            call_first(self.dds, ("data_transfer_mode",), dds_transfer_single, optional=True)
        dds_trigger_none = get_spcm_constant("SPCM_DDS_TRG_SRC_NONE", optional=True)
        if dds_trigger_none is not None:
            call_first(self.dds, ("trg_src",), dds_trigger_none, optional=True)
        call_first(self.dds, ("write_setup",), optional=True)
        self._dds_setup_done = True
        self._best_effort_route_cores()
        self.start_output()

    def _best_effort_route_cores(self) -> None:
        if self.dds is None:
            return
        core_masks = []
        for core_index in range(max(1, self.args.route_core_count)):
            core_mask = get_spcm_constant(f"SPCM_DDS_CORE{core_index}", optional=True)
            core_masks.append(int(core_mask) if core_mask is not None else (1 << core_index))
        try:
            call_first(
                self.dds,
                ("cores_on_channel", "assign_cores_to_channel"),
                self.args.output_channel,
                *core_masks,
                optional=True,
            )
        except Exception:
            pass

    def card_info(self) -> M4iCardInfo:
        product_name = safe_call_first(self.card, ("product_name", "name"))
        serial_number = safe_call_first(self.card, ("sn", "serial_number"))
        bits_per_sample = safe_call_first(self.card, ("bits_per_sample",))
        max_sample_value = safe_call_first(self.card, ("max_sample_value",))
        sample_rate_hz = self._read_card_int(("SPC_SAMPLERATE",))
        if sample_rate_hz is None:
            sample_rate_hz = safe_call_first(self.card, ("sample_rate", "samplerate"))
        onboard_memory_samples = self._read_card_int(("SPC_PCIMEMSIZE", "SPC_MEMSIZE"))
        dds_available = self.dds_mode_available()
        dds_queue_capacity = self.dds_queue_capacity() if dds_available else None
        return M4iCardInfo(
            identifier=self.args.card_identifier,
            product_name=str(product_name) if product_name is not None else None,
            serial_number=int(serial_number) if serial_number is not None else None,
            active_channel_mask=self.active_channel_mask,
            active_channel_count=self.active_channel_count,
            bits_per_sample=int(bits_per_sample) if bits_per_sample is not None else None,
            bytes_per_sample=self.bytes_per_sample,
            max_sample_value=int(max_sample_value) if max_sample_value is not None else None,
            sample_rate_hz=int(sample_rate_hz) if sample_rate_hz is not None else None,
            onboard_memory_samples=int(onboard_memory_samples) if onboard_memory_samples is not None else None,
            dds_queue_capacity=int(dds_queue_capacity) if dds_queue_capacity is not None else None,
            dds_available=dds_available,
        )

    def _read_card_int(self, constant_names: Sequence[str]) -> Optional[int]:
        for constant_name in constant_names:
            constant = get_spcm_constant(constant_name, optional=True)
            if constant is None:
                continue
            try:
                value = call_first(self.card, ("get_i", "get_i64", "get32", "get64"), constant, optional=True)
                if value is not None:
                    return int(value)
            except Exception:
                continue
        return None

    def card_mode_available(self, constant_name: str) -> bool:
        mode = get_spcm_constant(constant_name, optional=True)
        avail_modes = get_spcm_constant("SPC_AVAILCARDMODES", optional=True)
        if mode is None or avail_modes is None:
            return False
        try:
            value = call_first(self.card, ("get_i", "get_i64", "get32", "get64"), avail_modes, optional=False)
        except Exception:
            return False
        return bool(int(value) & int(mode))

    def dds_mode_available(self) -> bool:
        return self.card_mode_available("SPC_REP_STD_DDS")

    def dds_queue_capacity(self) -> Optional[int]:
        if self.dds is not None:
            try:
                value = call_first(
                    self.dds,
                    ("queue_cmd_max", "command_queue_max", "cmd_queue_max"),
                    optional=True,
                )
                if value is not None:
                    return int(value)
            except Exception:
                pass
        return self._read_card_int(("SPC_DDS_QUEUE_CMD_MAX",))

    def dds_queue_count(self) -> Optional[int]:
        if self.dds is not None:
            try:
                value = call_first(
                    self.dds,
                    ("queue_cmd_count", "command_queue_count", "cmd_queue_count"),
                    optional=True,
                )
                if value is not None:
                    return int(value)
            except Exception:
                pass
        return self._read_card_int(("SPC_DDS_QUEUE_CMD_COUNT",))

    def dds_write_to_card(self) -> None:
        self.ensure_dds()
        call_first(self.dds, ("write_to_card",), optional=False)

    def dds_exec_now(self) -> None:
        self.ensure_dds()
        call_first(self.dds, ("exec_now",), optional=False)

    def dds_exec_at_trig(self) -> None:
        self.ensure_dds()
        call_first(self.dds, ("exec_at_trig", "exec_at_trigger"), optional=False)

    def dds_commit_now(self) -> None:
        self.dds_exec_now()
        self.dds_write_to_card()

    def ensure_replay_continuous(self) -> None:
        self._set_card_mode_constant("SPC_REP_STD_CONTINUOUS")

    def memory_size_alignment_samples(self) -> int:
        step = self._read_card_int(("SPC_AVAILMEMSIZE_STEP",))
        return max(1, int(step or 1))

    def dds_core(self, index: int) -> Any:
        self.ensure_dds()
        try:
            return self.dds[index]
        except Exception as exc:
            raise RuntimeError(f"Could not access DDS core {index}") from exc

    def zero_cores(self, core_indices: Iterable[int]) -> None:
        for core_index in core_indices:
            core = self.dds_core(core_index)
            call_first(core, ("amp", "amplitude"), 0.0, optional=False)

    def configure_dense_update(
        self,
        base_freq_hz: float,
        active_cores: int,
        spacing_hz: float,
        amplitude_fraction: float,
        phase_stride_deg: float,
    ) -> int:
        self.ensure_dds()
        per_core_amp = amplitude_fraction / float(max(active_cores, 1))
        commands_per_update = 0
        center_offset = (active_cores - 1) / 2.0
        for core_index in range(active_cores):
            freq_hz = base_freq_hz + ((core_index - center_offset) * spacing_hz)
            phase_deg = phase_stride_deg * core_index
            core = self.dds_core(core_index)
            call_first(core, ("freq", "frequency"), float(freq_hz), optional=False)
            call_first(core, ("amp", "amplitude"), float(per_core_amp), optional=False)
            call_first(core, ("phase",), float(phase_deg), optional=False)
            commands_per_update += 3
        if active_cores < self.args.route_core_count:
            self.zero_cores(range(active_cores, self.args.route_core_count))
        return commands_per_update

    def configure_single_tone(
        self,
        freq_hz: float,
        amplitude_fraction: float,
        phase_deg: float = 0.0,
    ) -> int:
        return self.configure_dense_update(
            base_freq_hz=freq_hz,
            active_cores=1,
            spacing_hz=0.0,
            amplitude_fraction=amplitude_fraction,
            phase_stride_deg=phase_deg,
        )

    def apply_dense_update_now(
        self,
        base_freq_hz: float,
        active_cores: int,
        spacing_hz: float,
        amplitude_fraction: float,
        phase_stride_deg: float,
    ) -> int:
        commands = self.configure_dense_update(
            base_freq_hz=base_freq_hz,
            active_cores=active_cores,
            spacing_hz=spacing_hz,
            amplitude_fraction=amplitude_fraction,
            phase_stride_deg=phase_stride_deg,
        )
        self.dds_commit_now()
        return commands + 1

    def configure_dds_pulse_carrier(
        self,
        carrier_hz: float,
        amplitude_fraction: float,
    ) -> None:
        self.ensure_dds()
        core = self.dds_core(0)
        call_first(core, ("freq", "frequency"), float(carrier_hz), optional=False)
        call_first(core, ("phase",), 0.0, optional=False)
        call_first(core, ("amp", "amplitude"), float(amplitude_fraction), optional=False)
        if self.args.route_core_count > 1:
            self.zero_cores(range(1, self.args.route_core_count))
        self.dds_commit_now()

    def apply_dds_pulse_amplitude_now(self, amplitude_fraction: float) -> int:
        self.ensure_dds()
        core = self.dds_core(0)
        call_first(core, ("amp", "amplitude"), float(amplitude_fraction), optional=False)
        self.dds_commit_now()
        return 2

    def upload_waveform(self, waveform: Any) -> int:
        require_dependency(np, "numpy")
        data_transfer = spcm.DataTransfer(self.card)
        samples_per_channel = int(waveform.shape[-1])
        active_channels = max(1, self.active_channel_count)
        call_first(data_transfer, ("memory_size",), samples_per_channel, optional=False)
        call_first(data_transfer, ("allocate_buffer",), samples_per_channel, optional=False)

        buffer_view = getattr(data_transfer, "buffer", None)
        if buffer_view is None:
            raise RuntimeError("spcm.DataTransfer does not expose a writable buffer")

        if getattr(buffer_view, "ndim", 1) == 1:
            buffer_view[:] = waveform
        elif active_channels == 1:
            buffer_view[0, :] = waveform
        else:
            for channel_index in range(active_channels):
                buffer_view[channel_index, :] = waveform

        wait_cmd = get_spcm_constant("M2CMD_DATA_WAITDMA", optional=True)
        start_cmd = get_spcm_constant("M2CMD_DATA_STARTDMA", optional=True)
        if start_cmd is None:
            call_first(data_transfer, ("start_buffer_transfer",), optional=False)
        elif wait_cmd is None:
            call_first(data_transfer, ("start_buffer_transfer",), start_cmd, optional=False)
        else:
            call_first(
                data_transfer,
                ("start_buffer_transfer",),
                start_cmd,
                wait_cmd,
                optional=False,
            )
        return samples_per_channel * active_channels * self.bytes_per_sample

def waveform_sine_int16(
    samples: int,
    full_scale: int,
    cycles: int = 7,
) -> Any:
    require_dependency(np, "numpy")
    phase = np.arange(samples, dtype=np.float64) * (2.0 * math.pi * float(cycles) / float(samples))
    data = np.round(np.sin(phase) * full_scale).astype(np.int16)
    return data


def waveform_gated_sine_int16(
    period_samples: int,
    high_samples: int,
    cycles: int,
    sample_rate_hz: float,
    carrier_hz: float,
    full_scale: int,
) -> Any:
    require_dependency(np, "numpy")
    period_samples = max(2, int(period_samples))
    high_samples = max(1, min(int(high_samples), period_samples - 1))
    cycles = max(1, int(cycles))
    sample_count = period_samples * cycles
    sample_index = np.arange(sample_count, dtype=np.float64)
    envelope = ((sample_index.astype(np.int64) % period_samples) < high_samples).astype(np.float64)
    phase = sample_index * (2.0 * math.pi * float(carrier_hz) / float(sample_rate_hz))
    return np.round(np.sin(phase) * envelope * full_scale).astype(np.int16)


def waveform_square_pulse_int16(
    period_samples: int,
    high_samples: int,
    cycles: int,
    full_scale: int,
    high_fraction: float,
    low_fraction: float = 0.0,
) -> Any:
    require_dependency(np, "numpy")
    period_samples = max(2, int(period_samples))
    high_samples = max(1, min(int(high_samples), period_samples - 1))
    cycles = max(1, int(cycles))
    sample_count = period_samples * cycles
    sample_index = np.arange(sample_count, dtype=np.int64)
    logic = (sample_index % period_samples) < high_samples
    high_value = int(round(full_scale * float(high_fraction)))
    low_value = int(round(full_scale * float(low_fraction)))
    return np.where(logic, high_value, low_value).astype(np.int16)


def moving_average(values: Sequence[float], window_samples: int) -> List[float]:
    window_samples = max(1, int(window_samples))
    if window_samples <= 1 or len(values) <= 2:
        return [float(value) for value in values]
    output: List[float] = []
    running = 0.0
    queue: List[float] = []
    for value in values:
        running += float(value)
        queue.append(float(value))
        if len(queue) > window_samples:
            running -= queue.pop(0)
        output.append(running / float(len(queue)))
    return output


def analyze_scope_pulse_envelope(
    times_s: Sequence[float],
    volts: Sequence[float],
    threshold_fraction: float,
    smoothing_samples: int,
) -> dict:
    if len(times_s) != len(volts) or len(times_s) < 3:
        return {
            "status": "insufficient_samples",
            "measured_pulse_frequency_hz": None,
            "measured_duty_cycle_percent": None,
            "rising_edges": 0,
        }

    baseline = statistics.median(volts)
    envelope = moving_average([abs(value - baseline) for value in volts], smoothing_samples)
    low = min(envelope)
    high = max(envelope)
    if high <= low:
        return {
            "status": "flat_envelope",
            "measured_pulse_frequency_hz": None,
            "measured_duty_cycle_percent": None,
            "rising_edges": 0,
            "envelope_min_v": low,
            "envelope_max_v": high,
        }

    threshold_fraction = max(0.01, min(float(threshold_fraction), 0.99))
    threshold = low + ((high - low) * threshold_fraction)
    logic = [value >= threshold for value in envelope]
    rising_edge_times: List[float] = []
    high_durations: List[float] = []
    current_high_start: Optional[float] = times_s[0] if logic[0] else None

    for idx in range(1, len(logic)):
        if logic[idx] and not logic[idx - 1]:
            rising_edge_times.append(times_s[idx])
            current_high_start = times_s[idx]
        elif not logic[idx] and logic[idx - 1] and current_high_start is not None:
            high_durations.append(max(0.0, times_s[idx] - current_high_start))
            current_high_start = None

    if current_high_start is not None:
        high_durations.append(max(0.0, times_s[-1] - current_high_start))

    periods = [
        rising_edge_times[idx] - rising_edge_times[idx - 1]
        for idx in range(1, len(rising_edge_times))
        if rising_edge_times[idx] > rising_edge_times[idx - 1]
    ]
    measured_frequency_hz = None
    measured_period_s = None
    if periods:
        measured_period_s = statistics.median(periods)
        measured_frequency_hz = 1.0 / measured_period_s if measured_period_s > 0.0 else None

    measured_duty_cycle_percent = None
    if measured_period_s and high_durations:
        measured_duty_cycle_percent = 100.0 * statistics.median(high_durations) / measured_period_s

    return {
        "status": "ok" if measured_frequency_hz is not None else "no_period_detected",
        "measured_pulse_frequency_hz": measured_frequency_hz,
        "measured_period_s": measured_period_s,
        "measured_duty_cycle_percent": measured_duty_cycle_percent,
        "rising_edges": len(rising_edge_times),
        "period_count": len(periods),
        "threshold_v": threshold,
        "baseline_v": baseline,
        "envelope_min_v": low,
        "envelope_max_v": high,
    }


def analyze_scope_pulse_threshold(
    times_s: Sequence[float],
    volts: Sequence[float],
    threshold_fraction: float,
    smoothing_samples: int,
) -> dict:
    if len(times_s) != len(volts) or len(times_s) < 3:
        return {
            "status": "insufficient_samples",
            "measured_pulse_frequency_hz": None,
            "measured_duty_cycle_percent": None,
            "rising_edges": 0,
        }

    smoothed = moving_average(volts, smoothing_samples)
    low = min(smoothed)
    high = max(smoothed)
    if high <= low:
        return {
            "status": "flat_waveform",
            "measured_pulse_frequency_hz": None,
            "measured_duty_cycle_percent": None,
            "rising_edges": 0,
            "waveform_min_v": low,
            "waveform_max_v": high,
        }

    threshold_fraction = max(0.01, min(float(threshold_fraction), 0.99))
    threshold = low + ((high - low) * threshold_fraction)
    logic = [value >= threshold for value in smoothed]
    rising_edge_times: List[float] = []
    high_durations: List[float] = []
    current_high_start: Optional[float] = times_s[0] if logic[0] else None

    for idx in range(1, len(logic)):
        if logic[idx] and not logic[idx - 1]:
            rising_edge_times.append(times_s[idx])
            current_high_start = times_s[idx]
        elif not logic[idx] and logic[idx - 1] and current_high_start is not None:
            high_durations.append(max(0.0, times_s[idx] - current_high_start))
            current_high_start = None

    if current_high_start is not None:
        high_durations.append(max(0.0, times_s[-1] - current_high_start))

    periods = [
        rising_edge_times[idx] - rising_edge_times[idx - 1]
        for idx in range(1, len(rising_edge_times))
        if rising_edge_times[idx] > rising_edge_times[idx - 1]
    ]
    measured_frequency_hz = None
    measured_period_s = None
    if periods:
        measured_period_s = statistics.median(periods)
        measured_frequency_hz = 1.0 / measured_period_s if measured_period_s > 0.0 else None

    measured_duty_cycle_percent = None
    if measured_period_s and high_durations:
        measured_duty_cycle_percent = 100.0 * statistics.median(high_durations) / measured_period_s

    return {
        "status": "ok" if measured_frequency_hz is not None else "no_period_detected",
        "measured_pulse_frequency_hz": measured_frequency_hz,
        "measured_period_s": measured_period_s,
        "measured_duty_cycle_percent": measured_duty_cycle_percent,
        "rising_edges": len(rising_edge_times),
        "period_count": len(periods),
        "threshold_v": threshold,
        "waveform_min_v": low,
        "waveform_max_v": high,
    }


def save_scope_waveform_csv(path: Path, times_s: Sequence[float], volts: Sequence[float]) -> None:
    with path.open("w", encoding="utf-8", newline="") as fp:
        writer = csv.writer(fp)
        writer.writerow(["time_s", "voltage_v"])
        for time_s, voltage_v in zip(times_s, volts):
            writer.writerow([f"{time_s:.12e}", f"{voltage_v:.9e}"])


def ensure_output_dir(output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)


def build_analyzer_settings(args: argparse.Namespace) -> AnalyzerSettings:
    return AnalyzerSettings(
        rbw_hz=args.rbw_hz,
        vbw_hz=args.vbw_hz,
        sweep_count=args.sweep_count,
        trace_mode=args.trace_mode,
        detector=args.detector,
        reference_level_dbm=args.reference_level_dbm,
        display_range_db=args.display_range_db,
        attenuation_auto=args.attenuation_auto == "on",
        preamp_on=args.preamplifier == "on",
        impedance_ohms=args.input_impedance,
        capture_trace=args.capture_trace,
    )


def build_dynamic_settings(args: argparse.Namespace) -> AnalyzerSettings:
    return AnalyzerSettings(
        rbw_hz=args.dynamic_rbw_hz or args.rbw_hz,
        vbw_hz=args.dynamic_vbw_hz or args.vbw_hz,
        sweep_count=args.dynamic_sweep_count,
        trace_mode=args.dynamic_trace_mode,
        detector=args.dynamic_detector or args.detector,
        reference_level_dbm=args.reference_level_dbm,
        display_range_db=args.display_range_db,
        attenuation_auto=args.attenuation_auto == "on",
        preamp_on=args.preamplifier == "on",
        impedance_ohms=args.input_impedance,
        capture_trace=False,
    )


def build_sfdr_settings(args: argparse.Namespace) -> SfdrSettings:
    return SfdrSettings(
        search_start_hz=args.sfdr_start_hz,
        search_stop_hz=args.sfdr_stop_hz,
        carrier_guard_hz=args.sfdr_guard_hz,
    )


def sleep_settle(settle_s: float) -> None:
    if settle_s > 0.0:
        time.sleep(settle_s)


def capture_sfdr_step(
    analyzer: RohdeSchwarzFSH,
    output_dir: Path,
    step: StepSpec,
    settings: AnalyzerSettings,
    sfdr_settings: SfdrSettings,
    dump_analyzer_state: bool,
) -> StepCaptureSummary:
    metrics = analyzer.capture_sfdr(step, settings, sfdr_settings)
    csv_path = output_dir / f"step{step.index:02d}_{step.name}.csv"
    csv_path.write_text(
        "\n".join(
            [
                "carrier_power_dbm,carrier_freq_hz,spur_power_dbm,spur_freq_hz,sfdr_db",
                (
                    f"{metrics.power_dbm:.6f},"
                    f"{metrics.power_freq_hz:.6f},"
                    f"{'' if metrics.spur_power_dbm is None else f'{metrics.spur_power_dbm:.6f}'},"
                    f"{'' if metrics.spur_freq_hz is None else f'{metrics.spur_freq_hz:.6f}'},"
                    f"{'' if metrics.sfdr_db is None else f'{metrics.sfdr_db:.6f}'}"
                ),
            ]
        )
        + "\n",
        encoding="utf-8",
    )
    write_step_json(
        output_dir / f"step{step.index:02d}_{step.name}.json",
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
        csv_path=str(csv_path.resolve()),
        metrics=metrics,
    )
    print_step_summary(step, metrics)
    return summary


def capture_dynamic_step(
    analyzer: RohdeSchwarzFSH,
    output_dir: Path,
    step: DynamicRetuneSpec,
    settings: AnalyzerSettings,
    sfdr_settings: SfdrSettings,
    dump_analyzer_state: bool,
) -> StepCaptureSummary:
    metrics = capture_dynamic_retune_metrics(analyzer, step, settings, sfdr_settings)
    csv_path = output_dir / f"step{step.index:02d}_{step.name}.csv"
    with csv_path.open("w", encoding="utf-8", newline="") as fp:
        writer = csv.writer(fp)
        writer.writerow(["label", "search_left_hz", "search_right_hz", "power_dbm", "freq_hz"])
        for peak in metrics.intended_peaks + metrics.unintended_peaks:
            writer.writerow(
                [
                    peak.label,
                    f"{peak.search_left_hz:.6f}",
                    f"{peak.search_right_hz:.6f}",
                    "" if peak.power_dbm is None else f"{peak.power_dbm:.6f}",
                    "" if peak.freq_hz is None else f"{peak.freq_hz:.6f}",
                ]
            )
    step_json = StepSpec(
        group="dynamic_sfdr",
        index=step.index,
        name=step.name,
        marker=step.marker,
        description=step.description,
        expected_freq_hz=list(step.intended_freq_hz),
        span_hz=max(sfdr_settings.search_stop_hz - sfdr_settings.search_start_hz, 1.0),
        search_margin_hz=step.intended_margin_hz,
    )
    write_step_json(
        output_dir / f"step{step.index:02d}_{step.name}.json",
        analyzer.idn,
        step_json,
        metrics,
        extra=build_step_extra(
            analyzer,
            dump_analyzer_state,
            {
                "dwell_ms": step.dwell_ms,
                "transitions": step.transitions,
            },
        ),
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
    print_dynamic_summary(step, metrics)
    return summary


def benchmark_updates(
    controller: SpectrumM4i66Controller,
    name: str,
    updates: int,
    active_cores: int,
    base_freq_hz: float,
    step_freq_hz: float,
    spacing_hz: float,
    amplitude_fraction: float,
    phase_stride_deg: float,
    requested_interval_us: Optional[float],
) -> UpdateBenchmarkResult:
    latencies_us: List[float] = []
    intervals_us: List[float] = []
    queue_high_water = controller.dds_queue_count()
    notes: List[str] = []
    commands_per_update = (active_cores * 3) + 1

    controller.ensure_dds()
    controller.start_output()
    last_start_ns: Optional[int] = None
    schedule_deadline_s = time.perf_counter()
    burst_start_ns = time.perf_counter_ns()

    for update_index in range(updates):
        target_freq_hz = base_freq_hz + ((update_index % 2) * step_freq_hz)
        start_ns = time.perf_counter_ns()
        controller.apply_dense_update_now(
            base_freq_hz=target_freq_hz,
            active_cores=active_cores,
            spacing_hz=spacing_hz,
            amplitude_fraction=amplitude_fraction,
            phase_stride_deg=phase_stride_deg,
        )
        end_ns = time.perf_counter_ns()
        latencies_us.append((end_ns - start_ns) / 1000.0)

        if last_start_ns is not None:
            intervals_us.append((start_ns - last_start_ns) / 1000.0)
        last_start_ns = start_ns

        queue_count = controller.dds_queue_count()
        if queue_count is not None:
            queue_high_water = queue_count if queue_high_water is None else max(queue_high_water, queue_count)

        if requested_interval_us is not None:
            schedule_deadline_s += requested_interval_us / 1_000_000.0
            sleep_s = schedule_deadline_s - time.perf_counter()
            if sleep_s > 0.0:
                time.sleep(sleep_s)

    burst_elapsed_s = max((time.perf_counter_ns() - burst_start_ns) / 1e9, 1e-12)
    avg_latency_us = sum(latencies_us) / float(len(latencies_us))
    avg_interval_us = (sum(intervals_us) / float(len(intervals_us))) if intervals_us else None
    if requested_interval_us is not None and avg_interval_us is not None and avg_interval_us > (requested_interval_us * 1.25):
        notes.append("Requested cadence was not sustained; achieved interval drifted above target.")

    return UpdateBenchmarkResult(
        name=name,
        active_cores=active_cores,
        updates=updates,
        commands_per_update=commands_per_update,
        exec_mode="exec_now",
        requested_interval_us=requested_interval_us,
        avg_latency_us=avg_latency_us,
        min_latency_us=min(latencies_us),
        p50_latency_us=percentile(latencies_us, 50),
        p95_latency_us=percentile(latencies_us, 95),
        max_latency_us=max(latencies_us),
        achieved_update_rate_hz=updates / burst_elapsed_s,
        achieved_command_rate_hz=(updates * commands_per_update) / burst_elapsed_s,
        avg_interval_us=avg_interval_us,
        min_interval_us=min(intervals_us) if intervals_us else None,
        p95_interval_us=percentile(intervals_us, 95) if intervals_us else None,
        max_interval_us=max(intervals_us) if intervals_us else None,
        queue_high_water=queue_high_water,
        notes=notes,
    )


def probe_queue_capacity(
    controller: SpectrumM4i66Controller,
    active_cores: int,
    base_freq_hz: float,
    spacing_hz: float,
    amplitude_fraction: float,
    phase_stride_deg: float,
    max_updates: int,
) -> QueueProbeResult:
    controller.ensure_dds()
    controller.start_output()
    queue_capacity = controller.dds_queue_capacity()
    queue_high_water = controller.dds_queue_count()
    stopped_reason = "max_updates_reached"
    latencies_us: List[float] = []
    commands_per_update = (active_cores * 3) + 1
    start_ns = time.perf_counter_ns()
    queued_updates = 0

    try:
        call_first(controller.dds, ("reset", "clear"), optional=True)
        call_first(controller.dds, ("write_setup",), optional=True)
        controller._best_effort_route_cores()
        for update_index in range(max_updates):
            freq_hz = base_freq_hz + (update_index * spacing_hz)
            enqueue_start_ns = time.perf_counter_ns()
            controller.configure_dense_update(
                base_freq_hz=freq_hz,
                active_cores=active_cores,
                spacing_hz=spacing_hz,
                amplitude_fraction=amplitude_fraction,
                phase_stride_deg=phase_stride_deg,
            )
            controller.dds_exec_at_trig()
            controller.dds_write_to_card()
            enqueue_end_ns = time.perf_counter_ns()
            latencies_us.append((enqueue_end_ns - enqueue_start_ns) / 1000.0)
            queued_updates += 1
            queue_count = controller.dds_queue_count()
            if queue_count is not None:
                queue_high_water = queue_count if queue_high_water is None else max(queue_high_water, queue_count)
                if queue_capacity is not None and queue_count >= queue_capacity:
                    stopped_reason = "queue_capacity_reached"
                    break
    except Exception as exc:
        stopped_reason = f"exception:{exc}"

    elapsed_s = max((time.perf_counter_ns() - start_ns) / 1e9, 1e-12)
    return QueueProbeResult(
        name="dds_exec_at_trig_queue_probe",
        active_cores=active_cores,
        queued_updates=queued_updates,
        commands_per_update=commands_per_update,
        queue_capacity=queue_capacity,
        queue_high_water=queue_high_water,
        fill_elapsed_s=elapsed_s,
        avg_update_enqueue_us=(sum(latencies_us) / float(len(latencies_us))) if latencies_us else 0.0,
        achieved_command_rate_hz=((queued_updates * commands_per_update) / elapsed_s) if queued_updates else 0.0,
        stopped_reason=stopped_reason,
    )


def benchmark_memory_upload(
    controller: SpectrumM4i66Controller,
    name: str,
    samples_per_channel: int,
    transfers: int,
) -> MemoryBenchmarkResult:
    full_scale = max(1, int((2 ** (min(15, (controller.bytes_per_sample * 8) - 1))) - 1))
    waveform = waveform_sine_int16(samples_per_channel, full_scale=full_scale)
    latencies_us: List[float] = []
    bytes_transferred = 0

    start_ns = time.perf_counter_ns()
    for _ in range(transfers):
        transfer_start_ns = time.perf_counter_ns()
        bytes_transferred += controller.upload_waveform(waveform)
        transfer_end_ns = time.perf_counter_ns()
        latencies_us.append((transfer_end_ns - transfer_start_ns) / 1000.0)
    elapsed_s = max((time.perf_counter_ns() - start_ns) / 1e9, 1e-12)

    return MemoryBenchmarkResult(
        name=name,
        samples_per_channel=samples_per_channel,
        active_channels=controller.active_channel_count,
        transfers=transfers,
        bytes_per_sample=controller.bytes_per_sample,
        bytes_transferred=bytes_transferred,
        elapsed_s=elapsed_s,
        bandwidth_bytes_per_s=bytes_transferred / elapsed_s,
        avg_transfer_latency_us=sum(latencies_us) / float(len(latencies_us)),
        min_transfer_latency_us=min(latencies_us),
        p95_transfer_latency_us=percentile(latencies_us, 95),
        max_transfer_latency_us=max(latencies_us),
        notes=[],
    )


def clamp_pulse_samples(
    period_samples: int,
    duty_cycle_percent: float,
    min_high_samples: int,
    min_low_samples: int,
) -> tuple[int, int, int]:
    min_high_samples = max(1, int(min_high_samples))
    min_low_samples = max(1, int(min_low_samples))
    period_samples = max(int(period_samples), min_high_samples + min_low_samples)
    high_samples = int(round(period_samples * (duty_cycle_percent / 100.0)))
    high_samples = max(min_high_samples, min(high_samples, period_samples - min_low_samples))
    low_samples = period_samples - high_samples
    return period_samples, high_samples, low_samples


def lcm_int(a: int, b: int) -> int:
    a = abs(int(a))
    b = abs(int(b))
    if a == 0 or b == 0:
        return max(a, b)
    return (a // math.gcd(a, b)) * b


def align_replay_upload_cycles(
    requested_cycles: int,
    period_samples: int,
    max_cycles: Optional[int],
    sample_alignment: int,
) -> int:
    requested_cycles = max(1, int(requested_cycles))
    period_samples = max(1, int(period_samples))
    sample_alignment = max(1, int(sample_alignment))
    cycles_per_aligned_block = max(1, sample_alignment // math.gcd(sample_alignment, period_samples))
    upload_cycles = max(cycles_per_aligned_block, requested_cycles)
    upload_cycles = int(math.ceil(upload_cycles / cycles_per_aligned_block) * cycles_per_aligned_block)
    if max_cycles is not None and upload_cycles > max_cycles:
        upload_cycles = int(max_cycles // cycles_per_aligned_block) * cycles_per_aligned_block
        if upload_cycles < cycles_per_aligned_block:
            raise RuntimeError(
                "Replay upload cap is too small for one memory-aligned pulse waveform. "
                f"Need at least {cycles_per_aligned_block * period_samples} samples."
            )
    return max(cycles_per_aligned_block, upload_cycles)


def benchmark_dds_pulse_switching(
    controller: SpectrumM4i66Controller,
    card_info: M4iCardInfo,
    args: argparse.Namespace,
    scope: Optional[TektronixMSO22],
    output_dir: Path,
) -> PulseSwitchBenchmarkResult:
    sample_rate_hz = float(card_info.sample_rate_hz or args.sample_rate_hz)
    onboard_memory_samples = card_info.onboard_memory_samples
    latencies_us: List[float] = []
    notes = [
        "PulseGen is not used; live switching toggles DDS core amplitude with exec_now.",
        "Replay-memory fields describe an equivalent gated-sine pulse train.",
    ]

    controller.ensure_dds()
    controller.start_output()
    controller.configure_dds_pulse_carrier(
        carrier_hz=args.pulse_switch_carrier_hz,
        amplitude_fraction=args.pulse_switch_high_amplitude_fraction,
    )

    start_ns = time.perf_counter_ns()
    for toggle_index in range(max(1, args.pulse_switch_toggles)):
        amplitude = args.pulse_switch_high_amplitude_fraction if (toggle_index % 2) == 0 else 0.0
        toggle_start_ns = time.perf_counter_ns()
        controller.apply_dds_pulse_amplitude_now(amplitude)
        toggle_end_ns = time.perf_counter_ns()
        latencies_us.append((toggle_end_ns - toggle_start_ns) / 1000.0)
    elapsed_s = max((time.perf_counter_ns() - start_ns) / 1e9, 1e-12)

    edge_update_rate_hz = len(latencies_us) / elapsed_s
    pulse_cycle_frequency_hz = edge_update_rate_hz / 2.0
    min_period_samples = args.pulse_switch_min_high_samples + args.pulse_switch_min_low_samples
    min_period_samples = max(2, int(min_period_samples))
    max_replay_frequency_hz = sample_rate_hz / float(min_period_samples)
    replay_target_hz = args.pulse_switch_frequency_hz or pulse_cycle_frequency_hz
    ideal_period_samples = int(round(sample_rate_hz / max(replay_target_hz, 1e-12)))
    period_samples, high_samples, low_samples = clamp_pulse_samples(
        max(min_period_samples, ideal_period_samples),
        args.pulse_switch_duty_cycle_percent,
        args.pulse_switch_min_high_samples,
        args.pulse_switch_min_low_samples,
    )
    replay_frequency_hz = sample_rate_hz / float(period_samples)

    dds_queue_capacity = controller.dds_queue_capacity()
    queued_pulse_cycles_fit = None
    queued_duration_at_achieved_s = None
    if dds_queue_capacity is not None:
        queued_pulse_cycles_fit = int(dds_queue_capacity) // 2
        if pulse_cycle_frequency_hz > 0.0:
            queued_duration_at_achieved_s = queued_pulse_cycles_fit / pulse_cycle_frequency_hz

    cycles_fit_in_memory = None
    memory_duration_s = None
    continuous_loop_memory_limited = False
    if onboard_memory_samples is not None:
        cycles_fit_in_memory = int(onboard_memory_samples) // period_samples
        memory_duration_s = cycles_fit_in_memory / replay_frequency_hz
        continuous_loop_memory_limited = cycles_fit_in_memory < 1

    target_duration_s = args.pulse_switch_target_duration_s
    target_cycles = int(math.ceil(max(target_duration_s, 0.0) * replay_frequency_hz))
    samples_required = target_cycles * period_samples
    memory_limited_for_target = (
        None if onboard_memory_samples is None else samples_required > int(onboard_memory_samples)
    )

    upload_samples = None
    upload_bytes = None
    upload_elapsed_s = None
    upload_bandwidth = None
    if not args.skip_pulse_switch_upload:
        full_scale = max(1, int((2 ** (min(15, (controller.bytes_per_sample * 8) - 1))) - 1))
        if onboard_memory_samples is None:
            upload_cycles = max(1, args.pulse_switch_upload_cycles)
        else:
            max_upload_cycles = max(1, int(onboard_memory_samples) // period_samples)
            upload_cycles = min(max(1, args.pulse_switch_upload_cycles), max_upload_cycles)
        max_upload_cycles_by_samples = max(1, int(args.pulse_switch_upload_max_samples) // period_samples)
        upload_cycles = min(upload_cycles, max_upload_cycles_by_samples)
        waveform = waveform_gated_sine_int16(
            period_samples=period_samples,
            high_samples=high_samples,
            cycles=upload_cycles,
            sample_rate_hz=sample_rate_hz,
            carrier_hz=args.pulse_switch_carrier_hz,
            full_scale=full_scale,
        )
        upload_start_ns = time.perf_counter_ns()
        upload_bytes = controller.upload_waveform(waveform)
        upload_elapsed_s = max((time.perf_counter_ns() - upload_start_ns) / 1e9, 1e-12)
        upload_samples = int(waveform.shape[-1])
        upload_bandwidth = upload_bytes / upload_elapsed_s

    scope_capture = None
    if scope is not None:
        controller.configure_dds_pulse_carrier(
            carrier_hz=args.pulse_switch_carrier_hz,
            amplitude_fraction=args.pulse_switch_high_amplitude_fraction,
        )
        worker = DDSPulseSwitchWorker(
            controller=controller,
            high_amplitude_fraction=args.pulse_switch_high_amplitude_fraction,
            duration_s=args.scope_capture_s + 0.25,
        )
        worker.start()
        time.sleep(0.05)
        times_s, volts, scope_metadata = scope.capture_waveform(
            channel=args.scope_channel,
            capture_s=args.scope_capture_s,
            expected_frequency_hz=pulse_cycle_frequency_hz,
            vertical_scale_v=args.scope_vertical_scale_v,
        )
        worker.join()
        if worker.error:
            raise RuntimeError(f"DDS pulse scope worker failed: {worker.error}")
        analysis = analyze_scope_pulse_envelope(
            times_s=times_s,
            volts=volts,
            threshold_fraction=args.scope_envelope_threshold_fraction,
            smoothing_samples=args.scope_envelope_smoothing_samples,
        )
        waveform_path = None
        if args.scope_save_waveform:
            waveform_path = output_dir / "pulse_switch_scope_waveform.csv"
            save_scope_waveform_csv(waveform_path, times_s, volts)
        scope_capture = {
            **scope_metadata,
            **analysis,
            "worker_toggle_count": worker.toggle_count,
            "worker_edge_update_rate_hz": worker.toggle_count / max(worker.duration_s, 1e-12),
            "worker_avg_toggle_latency_us": (
                sum(worker.latencies_us) / float(len(worker.latencies_us))
                if worker.latencies_us
                else None
            ),
            "waveform_csv_path": str(waveform_path.resolve()) if waveform_path is not None else None,
            "measurement_note": (
                "The scope estimate thresholds the rectified waveform envelope; use a low carrier or "
                "external envelope/marker signal if automatic edge detection is ambiguous."
            ),
        }

    return PulseSwitchBenchmarkResult(
        name="dds_pulse_switching_limit",
        mode="dds_amp_exec_now_plus_replay_memory_model",
        status="ok",
        carrier_hz=args.pulse_switch_carrier_hz,
        requested_frequency_hz=args.pulse_switch_frequency_hz,
        achieved_pulse_cycle_frequency_hz=pulse_cycle_frequency_hz,
        achieved_edge_update_rate_hz=edge_update_rate_hz,
        max_replay_pulse_cycle_frequency_hz=max_replay_frequency_hz,
        replay_pulse_cycle_frequency_hz=replay_frequency_hz,
        sample_rate_hz=sample_rate_hz,
        period_samples=period_samples,
        high_samples=high_samples,
        low_samples=low_samples,
        duty_cycle_percent=(100.0 * high_samples / float(period_samples)),
        active_channels=controller.active_channel_count,
        onboard_memory_samples=onboard_memory_samples,
        dds_queue_capacity=dds_queue_capacity,
        queued_pulse_cycles_fit=queued_pulse_cycles_fit,
        queued_duration_at_achieved_s=queued_duration_at_achieved_s,
        cycles_fit_in_memory=cycles_fit_in_memory,
        memory_duration_s=memory_duration_s,
        target_duration_s=target_duration_s,
        samples_required_for_target_duration=samples_required,
        memory_limited_for_target_duration=memory_limited_for_target,
        continuous_loop_memory_limited=continuous_loop_memory_limited,
        upload_samples_per_channel=upload_samples,
        upload_bytes=upload_bytes,
        upload_elapsed_s=upload_elapsed_s,
        upload_bandwidth_bytes_per_s=upload_bandwidth,
        avg_toggle_latency_us=sum(latencies_us) / float(len(latencies_us)),
        min_toggle_latency_us=min(latencies_us),
        p50_toggle_latency_us=percentile(latencies_us, 50),
        p95_toggle_latency_us=percentile(latencies_us, 95),
        max_toggle_latency_us=max(latencies_us),
        scope_capture=scope_capture,
        error=None,
        notes=notes,
    )


def benchmark_replay_pulse_switching(
    controller: SpectrumM4i66Controller,
    card_info: M4iCardInfo,
    args: argparse.Namespace,
    scope: Optional[TektronixMSO22],
    output_dir: Path,
) -> PulseSwitchBenchmarkResult:
    sample_rate_hz = float(card_info.sample_rate_hz or args.sample_rate_hz)
    onboard_memory_samples = card_info.onboard_memory_samples
    min_period_samples = max(
        2,
        int(args.pulse_switch_min_high_samples) + int(args.pulse_switch_min_low_samples),
    )
    max_replay_frequency_hz = sample_rate_hz / float(min_period_samples)
    replay_target_hz = args.pulse_switch_frequency_hz or max_replay_frequency_hz
    ideal_period_samples = int(round(sample_rate_hz / max(replay_target_hz, 1e-12)))
    period_samples, high_samples, low_samples = clamp_pulse_samples(
        max(min_period_samples, ideal_period_samples),
        args.pulse_switch_duty_cycle_percent,
        args.pulse_switch_min_high_samples,
        args.pulse_switch_min_low_samples,
    )
    replay_frequency_hz = sample_rate_hz / float(period_samples)

    cycles_fit_in_memory = None
    memory_duration_s = None
    continuous_loop_memory_limited = False
    if onboard_memory_samples is not None:
        cycles_fit_in_memory = int(onboard_memory_samples) // period_samples
        memory_duration_s = cycles_fit_in_memory / replay_frequency_hz
        continuous_loop_memory_limited = cycles_fit_in_memory < 1

    target_duration_s = args.pulse_switch_target_duration_s
    target_cycles = int(math.ceil(max(target_duration_s, 0.0) * replay_frequency_hz))
    samples_required = target_cycles * period_samples
    memory_limited_for_target = (
        None if onboard_memory_samples is None else samples_required > int(onboard_memory_samples)
    )

    upload_samples = None
    upload_bytes = None
    upload_elapsed_s = None
    upload_bandwidth = None
    waveform = None
    if not args.skip_pulse_switch_upload or scope is not None:
        full_scale = max(1, int((2 ** (min(15, (controller.bytes_per_sample * 8) - 1))) - 1))
        memory_alignment_samples = controller.memory_size_alignment_samples()
        if onboard_memory_samples is None:
            max_upload_cycles_by_memory = None
        else:
            max_upload_cycles_by_memory = max(1, int(onboard_memory_samples) // period_samples)
        max_upload_cycles_by_samples = max(1, int(args.pulse_switch_upload_max_samples) // period_samples)
        max_upload_cycles = max_upload_cycles_by_samples
        if max_upload_cycles_by_memory is not None:
            max_upload_cycles = min(max_upload_cycles, max_upload_cycles_by_memory)
        upload_cycles = align_replay_upload_cycles(
            requested_cycles=args.pulse_switch_upload_cycles,
            period_samples=period_samples,
            max_cycles=max_upload_cycles,
            sample_alignment=memory_alignment_samples,
        )
        if args.pulse_switch_replay_waveform == "gated-sine":
            waveform = waveform_gated_sine_int16(
                period_samples=period_samples,
                high_samples=high_samples,
                cycles=upload_cycles,
                sample_rate_hz=sample_rate_hz,
                carrier_hz=args.pulse_switch_carrier_hz,
                full_scale=full_scale,
            )
        else:
            waveform = waveform_square_pulse_int16(
                period_samples=period_samples,
                high_samples=high_samples,
                cycles=upload_cycles,
                full_scale=full_scale,
                high_fraction=args.pulse_switch_high_amplitude_fraction,
            )
        controller.ensure_replay_continuous()
        upload_start_ns = time.perf_counter_ns()
        upload_bytes = controller.upload_waveform(waveform)
        upload_elapsed_s = max((time.perf_counter_ns() - upload_start_ns) / 1e9, 1e-12)
        upload_samples = int(waveform.shape[-1])
        upload_bandwidth = upload_bytes / upload_elapsed_s
        if upload_samples % memory_alignment_samples:
            raise RuntimeError(
                f"Internal error: replay upload length {upload_samples} is not aligned to "
                f"SPC_AVAILMEMSIZE_STEP={memory_alignment_samples}"
            )

    scope_capture = None
    if scope is not None:
        if waveform is None:
            raise RuntimeError("Replay pulse scope validation requires a replay waveform upload.")
        controller.start_output()
        time.sleep(0.05)
        times_s, volts, scope_metadata = scope.capture_waveform(
            channel=args.scope_channel,
            capture_s=args.scope_capture_s,
            expected_frequency_hz=replay_frequency_hz,
            vertical_scale_v=args.scope_vertical_scale_v,
        )
        if args.pulse_switch_replay_waveform == "gated-sine":
            analysis = analyze_scope_pulse_envelope(
                times_s=times_s,
                volts=volts,
                threshold_fraction=args.scope_envelope_threshold_fraction,
                smoothing_samples=args.scope_envelope_smoothing_samples,
            )
            measurement_note = (
                "Replay gated-sine scope estimate thresholds the rectified waveform envelope; "
                "use square replay waveform if RF-envelope detection is ambiguous."
            )
        else:
            analysis = analyze_scope_pulse_threshold(
                times_s=times_s,
                volts=volts,
                threshold_fraction=args.scope_envelope_threshold_fraction,
                smoothing_samples=args.scope_envelope_smoothing_samples,
            )
            measurement_note = "Replay square-pulse scope estimate thresholds the captured waveform directly."
        waveform_path = None
        if args.scope_save_waveform:
            waveform_path = output_dir / "pulse_switch_scope_waveform.csv"
            save_scope_waveform_csv(waveform_path, times_s, volts)
        scope_capture = {
            **scope_metadata,
            **analysis,
            "waveform_csv_path": str(waveform_path.resolve()) if waveform_path is not None else None,
            "measurement_note": measurement_note,
        }

    notes = [
        "DDS command mode is unavailable on this card/firmware; using AWG replay memory instead.",
        "PulseGen is not used.",
        "Replay mode measures generated pulse-train frequency and memory/upload limits, not host-driven DDS update latency.",
    ]
    if args.pulse_switch_replay_waveform == "square":
        notes.append("--pulse-switch-carrier-hz is ignored for square replay pulses.")
    if upload_samples is not None:
        notes.append(
            f"Replay upload samples were aligned to SPC_AVAILMEMSIZE_STEP={controller.memory_size_alignment_samples()}."
        )

    return PulseSwitchBenchmarkResult(
        name="replay_pulse_switching_limit",
        mode=f"awg_replay_{args.pulse_switch_replay_waveform}_pulse_train",
        status="ok",
        carrier_hz=(args.pulse_switch_carrier_hz if args.pulse_switch_replay_waveform == "gated-sine" else None),
        requested_frequency_hz=args.pulse_switch_frequency_hz,
        achieved_pulse_cycle_frequency_hz=replay_frequency_hz,
        achieved_edge_update_rate_hz=2.0 * replay_frequency_hz,
        max_replay_pulse_cycle_frequency_hz=max_replay_frequency_hz,
        replay_pulse_cycle_frequency_hz=replay_frequency_hz,
        sample_rate_hz=sample_rate_hz,
        period_samples=period_samples,
        high_samples=high_samples,
        low_samples=low_samples,
        duty_cycle_percent=(100.0 * high_samples / float(period_samples)),
        active_channels=controller.active_channel_count,
        onboard_memory_samples=onboard_memory_samples,
        dds_queue_capacity=None,
        queued_pulse_cycles_fit=None,
        queued_duration_at_achieved_s=None,
        cycles_fit_in_memory=cycles_fit_in_memory,
        memory_duration_s=memory_duration_s,
        target_duration_s=target_duration_s,
        samples_required_for_target_duration=samples_required,
        memory_limited_for_target_duration=memory_limited_for_target,
        continuous_loop_memory_limited=continuous_loop_memory_limited,
        upload_samples_per_channel=upload_samples,
        upload_bytes=upload_bytes,
        upload_elapsed_s=upload_elapsed_s,
        upload_bandwidth_bytes_per_s=upload_bandwidth,
        avg_toggle_latency_us=None,
        min_toggle_latency_us=None,
        p50_toggle_latency_us=None,
        p95_toggle_latency_us=None,
        max_toggle_latency_us=None,
        scope_capture=scope_capture,
        error=None,
        notes=notes,
    )


def benchmark_pulse_switching(
    controller: SpectrumM4i66Controller,
    card_info: M4iCardInfo,
    args: argparse.Namespace,
    scope: Optional[TektronixMSO22],
    output_dir: Path,
) -> PulseSwitchBenchmarkResult:
    if args.pulse_switch_mode == "dds":
        return benchmark_dds_pulse_switching(controller, card_info, args, scope, output_dir)
    if args.pulse_switch_mode == "replay" or not controller.dds_mode_available():
        return benchmark_replay_pulse_switching(controller, card_info, args, scope, output_dir)
    return benchmark_dds_pulse_switching(controller, card_info, args, scope, output_dir)


def write_update_results(output_dir: Path, results: Sequence[UpdateBenchmarkResult]) -> None:
    (output_dir / "update_benchmarks.json").write_text(
        json.dumps([asdict(item) for item in results], indent=2, default=json_default) + "\n",
        encoding="utf-8",
    )


def write_queue_probe(output_dir: Path, probe: QueueProbeResult) -> None:
    (output_dir / "queue_probe.json").write_text(
        json.dumps(asdict(probe), indent=2, default=json_default) + "\n",
        encoding="utf-8",
    )


def write_memory_results(output_dir: Path, results: Sequence[MemoryBenchmarkResult]) -> None:
    (output_dir / "memory_benchmarks.json").write_text(
        json.dumps([asdict(item) for item in results], indent=2, default=json_default) + "\n",
        encoding="utf-8",
    )


def write_pulse_switch_result(output_dir: Path, result: PulseSwitchBenchmarkResult) -> None:
    (output_dir / "pulse_switch_benchmark.json").write_text(
        json.dumps(asdict(result), indent=2, default=json_default) + "\n",
        encoding="utf-8",
    )


class DynamicBurstWorker(threading.Thread):
    def __init__(
        self,
        controller: SpectrumM4i66Controller,
        low_freq_hz: float,
        high_freq_hz: float,
        dwell_ms: int,
        transitions: int,
        amplitude_fraction: float,
    ):
        super().__init__(daemon=True)
        self.controller = controller
        self.low_freq_hz = low_freq_hz
        self.high_freq_hz = high_freq_hz
        self.dwell_ms = dwell_ms
        self.transitions = transitions
        self.amplitude_fraction = amplitude_fraction
        self.error: Optional[str] = None
        self.latencies_us: List[float] = []

    def run(self) -> None:
        try:
            schedule_deadline_s = time.perf_counter()
            for update_index in range(self.transitions):
                freq_hz = self.low_freq_hz if (update_index % 2) == 0 else self.high_freq_hz
                start_ns = time.perf_counter_ns()
                self.controller.apply_dense_update_now(
                    base_freq_hz=freq_hz,
                    active_cores=1,
                    spacing_hz=0.0,
                    amplitude_fraction=self.amplitude_fraction,
                    phase_stride_deg=0.0,
                )
                end_ns = time.perf_counter_ns()
                self.latencies_us.append((end_ns - start_ns) / 1000.0)
                schedule_deadline_s += self.dwell_ms / 1000.0
                sleep_s = schedule_deadline_s - time.perf_counter()
                if sleep_s > 0.0:
                    time.sleep(sleep_s)
        except Exception as exc:  # pragma: no cover - hardware-dependent
            self.error = str(exc)


class DDSPulseSwitchWorker(threading.Thread):
    def __init__(
        self,
        controller: SpectrumM4i66Controller,
        high_amplitude_fraction: float,
        duration_s: float,
    ):
        super().__init__(daemon=True)
        self.controller = controller
        self.high_amplitude_fraction = high_amplitude_fraction
        self.duration_s = max(0.01, float(duration_s))
        self.error: Optional[str] = None
        self.toggle_count = 0
        self.latencies_us: List[float] = []

    def run(self) -> None:
        deadline_s = time.perf_counter() + self.duration_s
        try:
            while time.perf_counter() < deadline_s:
                amplitude = self.high_amplitude_fraction if (self.toggle_count % 2) == 0 else 0.0
                start_ns = time.perf_counter_ns()
                self.controller.apply_dds_pulse_amplitude_now(amplitude)
                end_ns = time.perf_counter_ns()
                self.latencies_us.append((end_ns - start_ns) / 1000.0)
                self.toggle_count += 1
        except Exception as exc:  # pragma: no cover - hardware-dependent
            self.error = str(exc)


def run_dynamic_benchmarks(
    controller: SpectrumM4i66Controller,
    analyzer: RohdeSchwarzFSH,
    output_dir: Path,
    dynamic_steps: Sequence[DynamicRetuneSpec],
    dynamic_settings: AnalyzerSettings,
    sfdr_settings: SfdrSettings,
    amplitude_fraction: float,
    settle_before_capture_s: float,
    dump_analyzer_state: bool,
) -> tuple[List[StepCaptureSummary], List[dict]]:
    summaries: List[StepCaptureSummary] = []
    worker_summaries: List[dict] = []

    for step in dynamic_steps:
        worker = DynamicBurstWorker(
            controller=controller,
            low_freq_hz=step.intended_freq_hz[0],
            high_freq_hz=step.intended_freq_hz[1],
            dwell_ms=step.dwell_ms,
            transitions=step.transitions,
            amplitude_fraction=amplitude_fraction,
        )
        worker.start()
        time.sleep(min(settle_before_capture_s, max(0.05, step.dwell_ms / 1000.0)))
        summary = capture_dynamic_step(
            analyzer=analyzer,
            output_dir=output_dir,
            step=step,
            settings=dynamic_settings,
            sfdr_settings=sfdr_settings,
            dump_analyzer_state=dump_analyzer_state,
        )
        worker.join()
        if worker.error:
            raise RuntimeError(f"Dynamic burst worker failed: {worker.error}")
        summaries.append(summary)
        worker_summaries.append(
            {
                "name": step.name,
                "dwell_ms": step.dwell_ms,
                "transitions": step.transitions,
                "avg_update_latency_us": (
                    sum(worker.latencies_us) / float(len(worker.latencies_us))
                    if worker.latencies_us
                    else None
                ),
                "p95_update_latency_us": percentile(worker.latencies_us, 95) if worker.latencies_us else None,
                "max_update_latency_us": max(worker.latencies_us) if worker.latencies_us else None,
            }
        )

    return summaries, worker_summaries


def build_default_output_dir(args: argparse.Namespace) -> Path:
    if args.output_dir:
        return Path(args.output_dir)
    return Path("capture_runs") / f"m4i66_fsh_{utc_timestamp()}"


def parse_optional_sweep_range(
    start_hz: Optional[float],
    stop_hz: Optional[float],
    step_hz: Optional[float],
) -> Optional[List[float]]:
    values = [start_hz, stop_hz, step_hz]
    if all(value is None for value in values):
        return None
    if any(value is None for value in values):
        raise SystemExit("Sweep start/stop/step must be supplied together.")
    if step_hz <= 0.0 or stop_hz < start_hz:
        raise SystemExit("Sweep values are invalid.")
    return build_uniform_freq_list(start_hz, stop_hz, step_hz)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Benchmark a Spectrum M4i.66xx AWG with optional FSH analog tests and MSO22 pulse validation."
    )
    parser.add_argument(
        "--card-identifier",
        default="",
        help="Spectrum device identifier, for example /dev/spcm0 or TCPIP::192.168.1.10::inst0::INSTR",
    )
    parser.add_argument(
        "--channel-mask",
        type=lambda value: int(value, 0),
        default=0x1,
        help="Active channel bitmask for the AWG card, default 0x1",
    )
    parser.add_argument(
        "--output-channel",
        type=int,
        default=0,
        help="Logical output channel used for the routed DDS cores",
    )
    parser.add_argument(
        "--route-core-count",
        type=int,
        default=20,
        help="Number of DDS cores to keep under host control for dense benchmarks",
    )
    parser.add_argument(
        "--sample-rate-hz",
        type=float,
        default=625_000_000.0,
        help="AWG sample rate in Hz",
    )
    parser.add_argument(
        "--reference-clock-hz",
        type=float,
        default=0.0,
        help="Optional external reference clock in Hz",
    )
    parser.add_argument(
        "--channel-amplitude-mv",
        type=int,
        default=500,
        help="Output amplitude setting in mV",
    )
    parser.add_argument(
        "--output-load-ohms",
        type=int,
        default=50,
        help="Configured output load in ohms, use 0 to leave unchanged",
    )
    parser.add_argument(
        "--card-timeout-ms",
        type=int,
        default=30000,
        help="Spectrum card timeout in milliseconds",
    )
    parser.add_argument(
        "--assume-bytes-per-sample",
        type=int,
        default=2,
        help="Fallback bytes-per-sample if the driver does not expose it",
    )
    parser.add_argument(
        "--visa-resource",
        default="",
        help="PyVISA resource string for the FSH, for example TCPIP::192.168.100.142::INSTR",
    )
    parser.add_argument(
        "--visa-backend",
        default=None,
        help="Optional PyVISA backend, for example @py",
    )
    parser.add_argument(
        "--analyzer-timeout",
        type=float,
        default=30.0,
        help="Analyzer timeout in seconds",
    )
    parser.add_argument(
        "--analyzer-preset",
        choices=("off", "system", "reset"),
        default="off",
        help="Optional analyzer preset mode after connect",
    )
    parser.add_argument(
        "--scope-visa-resource",
        default="",
        help="Optional PyVISA resource string for the Tektronix MSO22 used by the pulse-switch bench",
    )
    parser.add_argument(
        "--scope-visa-backend",
        default=None,
        help='Optional PyVISA backend for the MSO22; use "@py" if default VISA reports the TCP/IP resource locked',
    )
    parser.add_argument(
        "--scope-timeout",
        type=float,
        default=30.0,
        help="MSO22 timeout in seconds",
    )
    parser.add_argument(
        "--scope-channel",
        default="CH1",
        help="MSO22 channel connected to the M4i66 pulse output",
    )
    parser.add_argument(
        "--scope-capture-s",
        type=float,
        default=0.2,
        help="MSO22 waveform capture duration used for pulse-switch validation",
    )
    parser.add_argument(
        "--scope-vertical-scale-v",
        type=float,
        default=0.2,
        help="MSO22 vertical scale in V/div for the pulse output channel",
    )
    parser.add_argument(
        "--scope-envelope-threshold-fraction",
        type=float,
        default=0.5,
        help="Envelope threshold fraction for pulse edge detection in captured scope data",
    )
    parser.add_argument(
        "--scope-envelope-smoothing-samples",
        type=int,
        default=16,
        help="Moving-average window for rectified scope-envelope edge detection",
    )
    parser.add_argument(
        "--scope-save-waveform",
        action="store_true",
        help="Save the MSO22 pulse-switch waveform as CSV",
    )
    parser.add_argument("--rbw-hz", type=float, default=100_000.0, help="FSH RBW in Hz")
    parser.add_argument("--vbw-hz", type=float, default=100_000.0, help="FSH VBW in Hz")
    parser.add_argument("--sweep-count", type=int, default=3, help="FSH sweep count")
    parser.add_argument(
        "--trace-mode",
        choices=("write", "average", "maxhold"),
        default="average",
        help="FSH trace mode",
    )
    parser.add_argument(
        "--detector",
        choices=("positive", "sample", "rms"),
        default="positive",
        help="FSH detector mode",
    )
    parser.add_argument(
        "--reference-level-dbm",
        type=float,
        default=0.0,
        help="FSH reference level in dBm",
    )
    parser.add_argument(
        "--display-range-db",
        type=float,
        default=80.0,
        help="FSH display range in dB",
    )
    parser.add_argument(
        "--attenuation-auto",
        choices=("on", "off"),
        default="on",
        help="FSH attenuation auto state",
    )
    parser.add_argument(
        "--preamplifier",
        choices=("on", "off"),
        default="off",
        help="FSH preamplifier state",
    )
    parser.add_argument(
        "--input-impedance",
        type=int,
        default=50,
        help="FSH input impedance in ohms",
    )
    parser.add_argument(
        "--capture-trace",
        action="store_true",
        help="Attempt raw FSH trace capture for DDS-band steps",
    )
    parser.add_argument(
        "--dump-analyzer-state",
        action="store_true",
        help="Include analyzer readback state in JSON artifacts",
    )
    parser.add_argument(
        "--skip-dds-band-test",
        action="store_true",
        help="Skip the steady-state DDS-band sweep",
    )
    parser.add_argument(
        "--skip-sfdr-test",
        action="store_true",
        help="Skip the steady-state SFDR sweep",
    )
    parser.add_argument(
        "--skip-dynamic-sfdr-test",
        action="store_true",
        help="Skip the host-driven dynamic retune FSH sweep",
    )
    parser.add_argument(
        "--skip-update-benchmarks",
        action="store_true",
        help="Skip the DDS control-path latency / rate benchmarks",
    )
    parser.add_argument(
        "--skip-queue-probe",
        action="store_true",
        help="Skip the dense DDS queue-pressure probe",
    )
    parser.add_argument(
        "--skip-memory-benchmarks",
        action="store_true",
        help="Skip the replay-memory upload bandwidth tests",
    )
    parser.add_argument(
        "--skip-pulse-switch-benchmark",
        action="store_true",
        help="Skip the DDS pulse-switching and replay-memory limit benchmark",
    )
    parser.add_argument(
        "--pulse-switch-mode",
        choices=("auto", "dds", "replay"),
        default="auto",
        help="Pulse-switch benchmark backend: auto uses DDS if card mode exists, otherwise AWG replay",
    )
    parser.add_argument(
        "--pulse-switch-replay-waveform",
        choices=("square", "gated-sine"),
        default="square",
        help="Replay fallback waveform; square is best for MSO22 pulse-frequency validation",
    )
    parser.add_argument(
        "--pulse-switch-carrier-hz",
        type=float,
        default=100_000_000.0,
        help="DDS/gated-sine carrier frequency used while toggling amplitude for pulse switching",
    )
    parser.add_argument(
        "--pulse-switch-frequency-hz",
        type=float,
        default=None,
        help="Optional pulse frequency to model/generate; replay default uses the maximum sample-granular rate",
    )
    parser.add_argument(
        "--pulse-switch-toggles",
        type=int,
        default=1000,
        help="Number of DDS high/low amplitude updates for the live pulse-switching benchmark",
    )
    parser.add_argument(
        "--pulse-switch-high-amplitude-fraction",
        type=float,
        default=0.6,
        help="DDS amplitude fraction for the high state; the low state is zero",
    )
    parser.add_argument(
        "--pulse-switch-duty-cycle-percent",
        type=float,
        default=50.0,
        help="Replay-memory pulse duty cycle used for memory-limit calculations",
    )
    parser.add_argument(
        "--pulse-switch-min-high-samples",
        type=int,
        default=1,
        help="Minimum high-state samples per replay-memory pulse",
    )
    parser.add_argument(
        "--pulse-switch-min-low-samples",
        type=int,
        default=1,
        help="Minimum low-state samples per replay-memory pulse",
    )
    parser.add_argument(
        "--pulse-switch-target-duration-s",
        type=float,
        default=1.0,
        help="Finite pulse-train duration used to decide whether replay memory is limiting",
    )
    parser.add_argument(
        "--pulse-switch-upload-cycles",
        type=int,
        default=65536,
        help="Replay pulse cycles to synthesize and upload as a representative memory-path probe",
    )
    parser.add_argument(
        "--pulse-switch-upload-max-samples",
        type=int,
        default=1_048_576,
        help="Safety cap on representative replay-pulse upload size per channel",
    )
    parser.add_argument(
        "--skip-pulse-switch-upload",
        action="store_true",
        help="Only compute replay-memory pulse limits; ignored when scope validation needs a replay waveform",
    )
    parser.add_argument(
        "--steady-amplitude-fraction",
        type=float,
        default=0.6,
        help="DDS amplitude fraction used for steady-state analog steps",
    )
    parser.add_argument(
        "--dense-amplitude-fraction",
        type=float,
        default=0.6,
        help="Total amplitude fraction budget for dense multi-core updates",
    )
    parser.add_argument(
        "--dense-spacing-hz",
        type=float,
        default=500_000.0,
        help="Per-core spacing in dense multi-core DDS workloads",
    )
    parser.add_argument(
        "--dense-phase-stride-deg",
        type=float,
        default=11.25,
        help="Per-core phase stride in dense multi-core DDS workloads",
    )
    parser.add_argument(
        "--dds-band-settle-ms",
        type=float,
        default=0.25,
        help="Settle delay after each steady-state tone update before FSH capture",
    )
    parser.add_argument(
        "--sfdr-settle-ms",
        type=float,
        default=0.25,
        help="Settle delay after each SFDR tone update before FSH capture",
    )
    parser.add_argument(
        "--dynamic-sweep-count",
        type=int,
        default=1,
        help="FSH sweep count for dynamic bursts",
    )
    parser.add_argument(
        "--dynamic-trace-mode",
        choices=("write", "average", "maxhold"),
        default="maxhold",
        help="FSH trace mode for dynamic bursts",
    )
    parser.add_argument(
        "--dynamic-detector",
        choices=("positive", "sample", "rms"),
        default=None,
        help="Optional detector override for dynamic bursts",
    )
    parser.add_argument(
        "--dynamic-rbw-hz",
        type=float,
        default=None,
        help="Optional RBW override for dynamic bursts",
    )
    parser.add_argument(
        "--dynamic-vbw-hz",
        type=float,
        default=None,
        help="Optional VBW override for dynamic bursts",
    )
    parser.add_argument(
        "--dynamic-pre-capture-settle-s",
        type=float,
        default=0.2,
        help="Delay after dynamic burst starts before the FSH dynamic capture begins",
    )
    parser.add_argument(
        "--sfdr-start-hz",
        type=float,
        default=5_000_000.0,
        help="SFDR search start frequency in Hz",
    )
    parser.add_argument(
        "--sfdr-stop-hz",
        type=float,
        default=1_000_000_000.0,
        help="SFDR search stop frequency in Hz",
    )
    parser.add_argument(
        "--sfdr-guard-hz",
        type=float,
        default=2_000_000.0,
        help="Carrier guard width in Hz for SFDR spur searches",
    )
    parser.add_argument(
        "--dds-band-sweep-start-hz",
        type=float,
        default=None,
        help="Optional custom DDS-band sweep start frequency in Hz",
    )
    parser.add_argument(
        "--dds-band-sweep-stop-hz",
        type=float,
        default=None,
        help="Optional custom DDS-band sweep stop frequency in Hz",
    )
    parser.add_argument(
        "--dds-band-sweep-step-hz",
        type=float,
        default=None,
        help="Optional custom DDS-band sweep step frequency in Hz",
    )
    parser.add_argument(
        "--sfdr-sweep-start-hz",
        type=float,
        default=None,
        help="Optional custom SFDR sweep start frequency in Hz",
    )
    parser.add_argument(
        "--sfdr-sweep-stop-hz",
        type=float,
        default=None,
        help="Optional custom SFDR sweep stop frequency in Hz",
    )
    parser.add_argument(
        "--sfdr-sweep-step-hz",
        type=float,
        default=None,
        help="Optional custom SFDR sweep step frequency in Hz",
    )
    parser.add_argument(
        "--single-update-count",
        type=int,
        default=200,
        help="Update count for the single-core latency/rate benchmark",
    )
    parser.add_argument(
        "--dense-update-count",
        type=int,
        default=200,
        help="Update count for the dense multi-core latency/rate benchmark",
    )
    parser.add_argument(
        "--paced-update-interval-us",
        type=float,
        default=1000.0,
        help="Requested update period for the paced dense update benchmark",
    )
    parser.add_argument(
        "--queue-probe-max-updates",
        type=int,
        default=512,
        help="Maximum queued updates to attempt in the DDS queue probe",
    )
    parser.add_argument(
        "--memory-bench-samples",
        type=int,
        default=8_388_608,
        help="Samples per channel for the large replay-memory upload benchmark",
    )
    parser.add_argument(
        "--memory-bench-repeats",
        type=int,
        default=2,
        help="Repeat count for the large replay-memory upload benchmark",
    )
    parser.add_argument(
        "--dense-memory-bench-samples",
        type=int,
        default=1_048_576,
        help="Samples per channel for the dense repeated upload benchmark",
    )
    parser.add_argument(
        "--dense-memory-bench-repeats",
        type=int,
        default=16,
        help="Repeat count for the dense repeated upload benchmark",
    )
    parser.add_argument(
        "--output-dir",
        default="",
        help="Directory for capture artifacts, defaults to capture_runs/m4i66_fsh_<timestamp>",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    output_dir = build_default_output_dir(args)
    ensure_output_dir(output_dir)

    analyzer = None
    scope = None
    controller = None
    try:
        analyzer_settings = build_analyzer_settings(args)
        dynamic_settings = build_dynamic_settings(args)
        sfdr_settings = build_sfdr_settings(args)

        dds_band_freqs = parse_optional_sweep_range(
            args.dds_band_sweep_start_hz,
            args.dds_band_sweep_stop_hz,
            args.dds_band_sweep_step_hz,
        ) or list(DEFAULT_DDS_BAND_FREQS_HZ)
        sfdr_freqs = parse_optional_sweep_range(
            args.sfdr_sweep_start_hz,
            args.sfdr_sweep_stop_hz,
            args.sfdr_sweep_step_hz,
        ) or list(DEFAULT_SFDR_FREQS_HZ)

        dds_band_steps = build_single_tone_step_specs(
            "dds_band",
            "M4I66-DDS-BAND",
            dds_band_freqs,
            "M4i66 DDS-band tone",
        )
        sfdr_steps = build_single_tone_step_specs(
            "sfdr",
            "M4I66-SFDR",
            sfdr_freqs,
            "M4i66 SFDR tone",
        )

        needs_analyzer = not (
            args.skip_dds_band_test
            and args.skip_sfdr_test
            and args.skip_dynamic_sfdr_test
        )
        if needs_analyzer:
            if not args.visa_resource:
                raise RuntimeError(
                    "--visa-resource is required unless DDS-band, SFDR, and dynamic SFDR are all skipped"
                )
            analyzer = RohdeSchwarzFSH(args.visa_resource, args.visa_backend, args.analyzer_timeout)
            analyzer.apply_preset(args.analyzer_preset)
            print(f"[HOST] Analyzer connected: {analyzer.idn}")
        if args.scope_visa_resource and not args.skip_pulse_switch_benchmark:
            scope = TektronixMSO22(
                args.scope_visa_resource,
                args.scope_visa_backend,
                args.scope_timeout,
            )
            print(f"[HOST] Scope connected: {scope.idn}")

        controller = SpectrumM4i66Controller(args)
        card_info = controller.card_info()
        print(
            "[HOST] AWG card ready: "
            f"product={card_info.product_name or 'unknown'} "
            f"serial={card_info.serial_number or 'unknown'} "
            f"channels={card_info.active_channel_count} "
            f"dds_available={card_info.dds_available}"
        )

        steps: List[StepCaptureSummary] = []
        update_results: List[UpdateBenchmarkResult] = []
        queue_probe: Optional[QueueProbeResult] = None
        memory_results: List[MemoryBenchmarkResult] = []
        pulse_switch_result: Optional[PulseSwitchBenchmarkResult] = None
        dynamic_worker_summaries: List[dict] = []

        needs_dds_command_mode = not (
            args.skip_dds_band_test
            and args.skip_sfdr_test
            and args.skip_dynamic_sfdr_test
            and args.skip_update_benchmarks
            and args.skip_queue_probe
        )
        if not args.skip_pulse_switch_benchmark and args.pulse_switch_mode == "dds":
            needs_dds_command_mode = True
        if needs_dds_command_mode:
            controller.ensure_dds()
            controller.start_output()

        if not args.skip_dds_band_test:
            print("[HOST] DDS-band sweep started.")
            for step in dds_band_steps:
                controller.apply_dense_update_now(
                    base_freq_hz=step.expected_freq_hz[0],
                    active_cores=1,
                    spacing_hz=0.0,
                    amplitude_fraction=args.steady_amplitude_fraction,
                    phase_stride_deg=0.0,
                )
                sleep_settle(args.dds_band_settle_ms / 1000.0)
                summary = capture_trace_step(
                    analyzer=analyzer,
                    output_dir=output_dir,
                    step=step,
                    settings=analyzer_settings,
                    dump_analyzer_state=args.dump_analyzer_state,
                )
                steps.append(summary)
                print_step_summary(step, summary.metrics)

        if not args.skip_sfdr_test:
            print("[HOST] SFDR sweep started.")
            for step in sfdr_steps:
                controller.apply_dense_update_now(
                    base_freq_hz=step.expected_freq_hz[0],
                    active_cores=1,
                    spacing_hz=0.0,
                    amplitude_fraction=args.steady_amplitude_fraction,
                    phase_stride_deg=0.0,
                )
                sleep_settle(args.sfdr_settle_ms / 1000.0)
                steps.append(
                    capture_sfdr_step(
                        analyzer=analyzer,
                        output_dir=output_dir,
                        step=step,
                        settings=analyzer_settings,
                        sfdr_settings=sfdr_settings,
                        dump_analyzer_state=args.dump_analyzer_state,
                    )
                )

        if not args.skip_dynamic_sfdr_test:
            print("[HOST] Dynamic SFDR bursts started.")
            dynamic_steps, dynamic_worker_summaries = run_dynamic_benchmarks(
                controller=controller,
                analyzer=analyzer,
                output_dir=output_dir,
                dynamic_steps=DEFAULT_DYNAMIC_STEPS,
                dynamic_settings=dynamic_settings,
                sfdr_settings=sfdr_settings,
                amplitude_fraction=args.steady_amplitude_fraction,
                settle_before_capture_s=args.dynamic_pre_capture_settle_s,
                dump_analyzer_state=args.dump_analyzer_state,
            )
            steps.extend(dynamic_steps)

        if not args.skip_update_benchmarks:
            print("[HOST] DDS update latency/rate benchmarks started.")
            update_results.append(
                benchmark_updates(
                    controller=controller,
                    name="dds_single_core_exec_now",
                    updates=args.single_update_count,
                    active_cores=1,
                    base_freq_hz=100_000_000.0,
                    step_freq_hz=300_000_000.0,
                    spacing_hz=0.0,
                    amplitude_fraction=args.steady_amplitude_fraction,
                    phase_stride_deg=0.0,
                    requested_interval_us=None,
                )
            )
            update_results.append(
                benchmark_updates(
                    controller=controller,
                    name="dds_dense_exec_now",
                    updates=args.dense_update_count,
                    active_cores=max(2, min(args.route_core_count, 20)),
                    base_freq_hz=120_000_000.0,
                    step_freq_hz=20_000_000.0,
                    spacing_hz=args.dense_spacing_hz,
                    amplitude_fraction=args.dense_amplitude_fraction,
                    phase_stride_deg=args.dense_phase_stride_deg,
                    requested_interval_us=None,
                )
            )
            update_results.append(
                benchmark_updates(
                    controller=controller,
                    name="dds_dense_paced_exec_now",
                    updates=args.dense_update_count,
                    active_cores=max(2, min(args.route_core_count, 20)),
                    base_freq_hz=120_000_000.0,
                    step_freq_hz=20_000_000.0,
                    spacing_hz=args.dense_spacing_hz,
                    amplitude_fraction=args.dense_amplitude_fraction,
                    phase_stride_deg=args.dense_phase_stride_deg,
                    requested_interval_us=args.paced_update_interval_us,
                )
            )
            write_update_results(output_dir, update_results)

        if not args.skip_queue_probe:
            print("[HOST] DDS queue-capacity probe started.")
            queue_probe = probe_queue_capacity(
                controller=controller,
                active_cores=max(2, min(args.route_core_count, 20)),
                base_freq_hz=150_000_000.0,
                spacing_hz=args.dense_spacing_hz,
                amplitude_fraction=args.dense_amplitude_fraction,
                phase_stride_deg=args.dense_phase_stride_deg,
                max_updates=args.queue_probe_max_updates,
            )
            write_queue_probe(output_dir, queue_probe)

        if not args.skip_memory_benchmarks:
            print("[HOST] Replay-memory bandwidth benchmarks started.")
            memory_results.append(
                benchmark_memory_upload(
                    controller=controller,
                    name="full_memory_upload",
                    samples_per_channel=args.memory_bench_samples,
                    transfers=args.memory_bench_repeats,
                )
            )
            memory_results.append(
                benchmark_memory_upload(
                    controller=controller,
                    name="dense_repeated_upload",
                    samples_per_channel=args.dense_memory_bench_samples,
                    transfers=args.dense_memory_bench_repeats,
                )
            )
            write_memory_results(output_dir, memory_results)

        if not args.skip_pulse_switch_benchmark:
            print("[HOST] Pulse-switching benchmark started.")
            pulse_switch_result = benchmark_pulse_switching(
                controller=controller,
                card_info=card_info,
                args=args,
                scope=scope,
                output_dir=output_dir,
            )
            write_pulse_switch_result(output_dir, pulse_switch_result)
            print(
                "[HOST] Pulse-switching summary: "
                f"edge_rate={pulse_switch_result.achieved_edge_update_rate_hz:.3f} Hz, "
                f"pulse_cycle_rate={pulse_switch_result.achieved_pulse_cycle_frequency_hz:.3f} Hz, "
                f"max_replay_rate={pulse_switch_result.max_replay_pulse_cycle_frequency_hz:.3f} Hz, "
                f"memory_limited_for_{pulse_switch_result.target_duration_s:.3f}s="
                f"{pulse_switch_result.memory_limited_for_target_duration}"
            )

        summary = {
            "timestamp_utc": utc_timestamp(),
            "analyzer_idn": analyzer.idn if analyzer is not None else None,
            "scope_idn": scope.idn if scope is not None else None,
            "card": asdict(card_info),
            "sample_rate_hz": args.sample_rate_hz,
            "analyzer_settings": asdict(analyzer_settings),
            "dynamic_analyzer_settings": asdict(dynamic_settings),
            "sfdr_settings": asdict(sfdr_settings),
            "dds_band_step_count": len(dds_band_steps),
            "sfdr_step_count": len(sfdr_steps),
            "dynamic_step_count": 0 if args.skip_dynamic_sfdr_test else len(DEFAULT_DYNAMIC_STEPS),
            "steps": [asdict(step) for step in steps],
            "update_benchmarks": [asdict(item) for item in update_results],
            "queue_probe": asdict(queue_probe) if queue_probe is not None else None,
            "memory_benchmarks": [asdict(item) for item in memory_results],
            "pulse_switch_benchmark": (
                asdict(pulse_switch_result) if pulse_switch_result is not None else None
            ),
            "dynamic_worker_summaries": dynamic_worker_summaries,
            "notes": [
                "Update latency values are host-to-card commit timings.",
                "Analog traces and SFDR values come from the R&S FSH analyzer path.",
                "Dense DDS queue behavior depends on the installed DDS option and spcm package version.",
                "Pulse switching uses DDS amplitude updates because PulseGen is not enabled.",
            ],
            "source_refs": [
                "Spectrum Instrumentation M4i.66xx datasheet",
                "Spectrum Instrumentation M4i/M4x 66xx/96xx user manual",
                "Spectrum Instrumentation spcm examples and DDS command-queue notes",
            ],
        }
        (output_dir / "summary.json").write_text(
            json.dumps(summary, indent=2, default=json_default) + "\n",
            encoding="utf-8",
        )

        write_sfdr_results_csv(steps, output_dir / "sfdr_results.csv")
        write_dynamic_results_csv(steps, output_dir / "dynamic_sfdr_results.csv")
        print(f"[HOST] Capture complete. Artifacts written to: {output_dir}")
        return 0
    except Exception as exc:
        print(f"[HOST] ERROR: {exc}")
        return 1
    finally:
        if controller is not None:
            try:
                controller.close()
            except Exception:
                pass
        if analyzer is not None:
            try:
                analyzer.close()
            except Exception:
                pass
        if scope is not None:
            try:
                scope.close()
            except Exception:
                pass


if __name__ == "__main__":
    raise SystemExit(main())
