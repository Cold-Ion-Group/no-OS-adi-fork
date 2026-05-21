#!/usr/bin/env python3
"""
Probe R&S FSH MMEM/file-export compatibility.

This intentionally tries multiple SCPI spellings because older FSH firmware
often differs from the generic R&S trace-transfer examples. The script records
exact command outcomes and error queues; it does not assume any candidate works.
"""

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path
from typing import Any, Dict, List, Optional

try:
    import pyvisa
except ImportError:  # pragma: no cover
    pyvisa = None


def drain_errors(inst: Any) -> List[str]:
    errors: List[str] = []
    for _ in range(16):
        try:
            text = inst.query("SYST:ERR?").strip()
        except Exception as exc:  # pragma: no cover - bench dependent
            errors.append(f"SYST:ERR? failed: {exc}")
            break
        errors.append(text)
        if text.startswith("0,"):
            break
    return errors


def write_record(inst: Any, command: str, *, settle_s: float = 0.0) -> Dict[str, Any]:
    record: Dict[str, Any] = {
        "command": command,
        "type": "write",
        "ok": False,
        "error": None,
        "post_errors": [],
    }
    try:
        inst.write("*CLS")
        inst.write(command)
        if settle_s > 0:
            time.sleep(settle_s)
        record["ok"] = True
    except Exception as exc:
        record["error"] = str(exc)
    record["post_errors"] = drain_errors(inst)
    return record


def query_record(inst: Any, command: str, *, binary: bool = False) -> Dict[str, Any]:
    record: Dict[str, Any] = {
        "command": command,
        "type": "query_binary" if binary else "query",
        "ok": False,
        "error": None,
        "response_len": 0,
        "response_preview": None,
        "post_errors": [],
    }
    try:
        inst.write("*CLS")
        if binary:
            data = inst.query_binary_values(command, datatype="B", container=bytes)
            record["response_len"] = len(data)
            record["response_preview"] = data[:120].hex()
        else:
            text = inst.query(command)
            record["response_len"] = len(text)
            record["response_preview"] = text[:240]
        record["ok"] = True
    except Exception as exc:
        record["error"] = str(exc)
    record["post_errors"] = drain_errors(inst)
    return record


def configure_simple_spectrum(inst: Any, center_hz: float, span_hz: float, rbw_hz: float, vbw_hz: float) -> List[Dict[str, Any]]:
    commands = [
        "*CLS",
        "DISP:TRAC:Y:RLEV -60dBm",
        "INP:ATT:AUTO ON",
        "INP:GAIN:STAT ON",
        "BAND:AUTO OFF",
        f"BAND {rbw_hz}",
        "BAND:VID:AUTO OFF",
        f"BAND:VID {vbw_hz}",
        "SWE:TIME:AUTO ON",
        "SWE:COUN 1",
        "DISP:WIND:TRAC:MODE WRIT",
        "DET POS",
        f"FREQ:CENT {center_hz}",
        f"FREQ:SPAN {span_hz}",
        "INIT:CONT OFF",
        "INIT;*WAI",
        "CALC:MARK1 ON",
        "CALC:MARK1:MAX",
    ]
    records = []
    for command in commands:
        try:
            inst.write(command)
            records.append({"command": command, "ok": True, "error": None, "post_errors": drain_errors(inst)})
        except Exception as exc:
            records.append({"command": command, "ok": False, "error": str(exc), "post_errors": drain_errors(inst)})
            break
    return records


def main() -> int:
    parser = argparse.ArgumentParser(description="Probe FSH MMEM/file-export SCPI compatibility.")
    parser.add_argument("--visa-resource", required=True)
    parser.add_argument("--visa-backend", default=None)
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--center-hz", type=float, default=200_000_000.0)
    parser.add_argument("--span-hz", type=float, default=1_000_000.0)
    parser.add_argument("--rbw-hz", type=float, default=10_000.0)
    parser.add_argument("--vbw-hz", type=float, default=10_000.0)
    parser.add_argument("--remote-file", default="trace_probe.csv")
    parser.add_argument("--output", default="")
    args = parser.parse_args()

    if pyvisa is None:
        raise SystemExit("pyvisa is required")

    rm = pyvisa.ResourceManager(args.visa_backend) if args.visa_backend else pyvisa.ResourceManager()
    inst = rm.open_resource(args.visa_resource)
    try:
        inst.timeout = int(args.timeout * 1000)
        inst.chunk_size = 1024 * 1024
        inst.write_termination = "\n"
        inst.read_termination = "\n"

        result: Dict[str, Any] = {
            "idn": inst.query("*IDN?").strip(),
            "resource": args.visa_resource,
            "visa_backend": args.visa_backend,
            "timeout_s": args.timeout,
            "remote_file": args.remote_file,
            "configure": configure_simple_spectrum(
                inst,
                args.center_hz,
                args.span_hz,
                args.rbw_hz,
                args.vbw_hz,
            ),
            "directory_queries": [],
            "save_candidates": [],
            "readback_candidates": [],
        }

        directory_queries = [
            "MMEM:CDIR?",
            "MMEM:CAT?",
            "MMEM:CAT? '.'",
            "MMEM:CAT? '/'",
            "MMEM:CAT? 'C:\\'",
            "MMEM:CAT? 'D:\\'",
            "MMEM:MSIS?",
        ]
        result["directory_queries"] = [query_record(inst, command) for command in directory_queries]

        quoted = f"'{args.remote_file}'"
        save_candidates = [
            f"MMEM:STOR:TRAC {quoted}",
            f"MMEM:STOR:TRACe {quoted}",
            f"MMEM:STOR:TRAC 1,{quoted}",
            f"MMEM:STOR:TRACe 1,{quoted}",
            f"MMEM:STOR:TRACe:DATA {quoted}",
            f"MMEM:STOR:TRACe:DATA 1,{quoted}",
            f"HCOP:DEST {quoted}",
            f"HCOP:IMM",
        ]
        result["save_candidates"] = [write_record(inst, command, settle_s=0.5) for command in save_candidates]

        readback_candidates = [
            f"MMEM:DATA? {quoted}",
            f"MMEM:DATA? './{args.remote_file}'",
            f"MMEM:DATA? '/{args.remote_file}'",
            f"MMEM:DATA? 'C:\\{args.remote_file}'",
            f"MMEM:DATA? 'D:\\{args.remote_file}'",
        ]
        result["readback_candidates"] = [query_record(inst, command, binary=False) for command in readback_candidates]
    finally:
        try:
            inst.close()
        finally:
            rm.close()

    text = json.dumps(result, indent=2) + "\n"
    if args.output:
        Path(args.output).write_text(text, encoding="utf-8")
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
