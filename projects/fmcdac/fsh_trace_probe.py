#!/usr/bin/env python3
"""
Minimal FSH trace-transfer probe.

Purpose:
1. connect to the FSH over VISA
2. configure one simple spectrum measurement
3. verify marker reads
4. try the trace export path directly
5. report any SYST:ERR? / SYST:ERR:ALL? output
"""

from __future__ import annotations

import argparse
import json
from dataclasses import asdict
from pathlib import Path

from run_nco_scope_test import AnalyzerSettings, RohdeSchwarzFSH, StepSpec, json_default


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Probe R&S FSH trace export over VISA.")
    parser.add_argument("--visa-resource", required=True)
    parser.add_argument("--visa-backend", default=None)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--center-hz", type=float, default=400_000_000.0)
    parser.add_argument("--span-hz", type=float, default=1_000_000.0)
    parser.add_argument("--rbw-hz", type=float, default=10_000.0)
    parser.add_argument("--vbw-hz", type=float, default=10_000.0)
    parser.add_argument("--sweep-count", type=int, default=1)
    parser.add_argument("--trace-mode", default="average", choices=["write", "average", "maxhold"])
    parser.add_argument("--detector", default="rms", choices=["positive", "sample", "rms"])
    parser.add_argument("--reference-level-dbm", type=float, default=0.0)
    parser.add_argument("--display-range-db", type=float, default=80.0)
    parser.add_argument("--impedance-ohms", type=int, default=50)
    parser.add_argument("--output", default="")
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    step = StepSpec(
        group="probe",
        index=1,
        name="fsh_trace_probe",
        marker="",
        description="Direct FSH trace probe",
        expected_freq_hz=[args.center_hz],
        span_hz=args.span_hz,
        search_margin_hz=args.span_hz / 2.0,
    )
    settings = AnalyzerSettings(
        rbw_hz=args.rbw_hz,
        vbw_hz=args.vbw_hz,
        sweep_count=args.sweep_count,
        trace_mode=args.trace_mode,
        detector=args.detector,
        reference_level_dbm=args.reference_level_dbm,
        display_range_db=args.display_range_db,
        attenuation_auto=True,
        preamp_on=False,
        impedance_ohms=args.impedance_ohms,
        capture_trace=True,
    )

    analyzer = RohdeSchwarzFSH(args.visa_resource, args.visa_backend, args.timeout)
    try:
        peak_power_dbm, peak_freq_hz = analyzer._capture_peak_for_span(args.center_hz, args.span_hz, settings)
        result = {
            "analyzer_idn": analyzer.idn,
            "center_hz": args.center_hz,
            "span_hz": args.span_hz,
            "peak_power_dbm": peak_power_dbm,
            "peak_freq_hz": peak_freq_hz,
            "settings": asdict(settings),
            "trace_success": False,
            "trace_points": 0,
            "trace_error": None,
            "system_errors": [],
        }
        try:
            trace_freqs_hz, trace_levels_dbm = analyzer._capture_trace_data(args.center_hz, args.span_hz)
            result["trace_success"] = True
            result["trace_points"] = len(trace_levels_dbm)
            result["trace_first_freq_hz"] = trace_freqs_hz[0] if trace_freqs_hz else None
            result["trace_last_freq_hz"] = trace_freqs_hz[-1] if trace_freqs_hz else None
            result["trace_first_power_dbm"] = trace_levels_dbm[0] if trace_levels_dbm else None
            result["trace_last_power_dbm"] = trace_levels_dbm[-1] if trace_levels_dbm else None
        except Exception as exc:
            result["trace_error"] = str(exc)
            result["system_errors"] = analyzer.query_system_errors()
    finally:
        analyzer.close()

    text = json.dumps(result, indent=2, default=json_default) + "\n"
    if args.output:
        Path(args.output).write_text(text, encoding="utf-8")
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
