#!/usr/bin/env python3
"""Standalone AWG scheduler console runner for FMCDAC."""

from __future__ import annotations

import argparse
from types import SimpleNamespace

from run_nco_scope_test import (
    run_awg_scheduler_console_mode,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Upload a host-built AWG scheduler event table and execute it over UART."
    )
    parser.add_argument("--serial-port", required=True, help="UART COM port, e.g. COM4")
    parser.add_argument("--baudrate", type=int, default=115200, help="UART baud rate")
    parser.add_argument(
        "--uart-timeout",
        type=float,
        default=120.0,
        help="Seconds to wait for scheduler console responses.",
    )
    parser.add_argument(
        "--xilinx-settings",
        action="append",
        default=[],
        help="Path to settings64.bat (repeatable).",
    )
    parser.add_argument(
        "--make-args",
        default="",
        help="Extra arguments appended to 'make run'.",
    )
    parser.add_argument(
        "--skip-make-run",
        action="store_true",
        help="Skip make run and just attach to UART.",
    )
    parser.add_argument(
        "--make-timeout",
        type=float,
        default=300.0,
        help="Timeout for 'make run' in seconds",
    )
    parser.add_argument(
        "--output-dir",
        default=None,
        help="Optional output directory (defaults to capture_runs/).",
    )
    parser.add_argument(
        "--visa-resource",
        default=None,
        help="Optional PyVISA resource string for the FSH8, for example TCPIP::192.168.100.135::INSTR.",
    )
    parser.add_argument(
        "--visa-backend",
        default=None,
        help="Optional PyVISA backend, for example @py.",
    )
    parser.add_argument(
        "--analyzer-timeout",
        type=float,
        default=30.0,
        help="Analyzer VISA timeout in seconds.",
    )
    parser.add_argument(
        "--analyzer-preset",
        choices=["off", "system", "reset"],
        default="off",
        help="Apply an analyzer-wide preset/reset after connect.",
    )
    parser.add_argument(
        "--dump-analyzer-state",
        action="store_true",
        help="Attach analyzer readback state snapshots to per-step JSON artifacts.",
    )
    parser.add_argument(
        "--rbw-hz",
        type=float,
        default=100_000.0,
        help="FSH8 resolution bandwidth in Hz.",
    )
    parser.add_argument(
        "--vbw-hz",
        type=float,
        default=100_000.0,
        help="FSH8 video bandwidth in Hz.",
    )
    parser.add_argument(
        "--sweep-count",
        type=int,
        default=3,
        help="Number of sweeps per AWG step measurement.",
    )
    parser.add_argument(
        "--trace-mode",
        choices=["average", "maxhold", "write"],
        default="average",
        help="FSH8 trace mode.",
    )
    parser.add_argument(
        "--detector",
        choices=["positive", "rms", "sample"],
        default="positive",
        help="FSH8 detector mode.",
    )
    parser.add_argument(
        "--reference-level-dbm",
        type=float,
        default=0.0,
        help="FSH8 display reference level in dBm.",
    )
    parser.add_argument(
        "--display-range-db",
        type=float,
        default=80.0,
        help="FSH8 display range in dB.",
    )
    parser.add_argument(
        "--attenuation-auto",
        choices=["on", "off"],
        default="on",
        help="Leave FSH8 attenuation coupled to the reference level.",
    )
    parser.add_argument(
        "--preamplifier",
        choices=["on", "off"],
        default="off",
        help="Enable or disable the FSH8 preamplifier.",
    )
    parser.add_argument(
        "--input-impedance",
        choices=["50", "75"],
        default="50",
        help="FSH8 input impedance setting.",
    )
    parser.add_argument(
        "--capture-trace",
        action="store_true",
        help="Also capture trace data for each AWG sweep step.",
    )
    parser.add_argument(
        "--awg-sweep-start-hz",
        type=float,
        required=True,
        help="AWG sweep start frequency in Hz.",
    )
    parser.add_argument(
        "--awg-sweep-stop-hz",
        type=float,
        required=True,
        help="AWG sweep stop frequency in Hz.",
    )
    parser.add_argument(
        "--awg-sweep-step-hz",
        type=float,
        required=True,
        help="AWG sweep step frequency in Hz.",
    )
    parser.add_argument(
        "--awg-sweep-dwell-us",
        type=int,
        default=None,
        help="AWG sweep dwell per step in microseconds. Defaults to a safe host-selected value.",
    )
    parser.add_argument(
        "--awg-sweep-scale-u",
        type=int,
        default=700000,
        help="AWG sweep DDS scale in micro-units.",
    )
    parser.add_argument(
        "--awg-sweep-start-ticks",
        type=int,
        default=None,
        help="AWG sweep start tick offset. Defaults to a safe host-computed startup margin.",
    )
    parser.add_argument(
        "--awg-sweep-tone",
        type=int,
        default=0,
        help="AWG sweep tone index.",
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
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    shim = SimpleNamespace(**vars(args))
    shim.run_awg_sweep = True
    shim.serial_dtr = "leave"
    shim.serial_rts = "leave"
    shim.list_visa = False
    shim.run_full_integration = False
    return run_awg_scheduler_console_mode(shim)


if __name__ == "__main__":
    raise SystemExit(main())
