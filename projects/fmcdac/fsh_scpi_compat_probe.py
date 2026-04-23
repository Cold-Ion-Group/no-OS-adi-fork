#!/usr/bin/env python3
"""
Probe FSH SCPI compatibility one command at a time.

Purpose:
1. isolate which specific commands are unsupported on older firmware
2. separate "undefined header" from plain trace-transfer timeout
3. compare native VISA vs pyvisa-py with the same sequence
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any, Dict, List

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


def run_case(inst: Any, name: str, setup: List[str], action: Dict[str, Any]) -> Dict[str, Any]:
    record: Dict[str, Any] = {
        "name": name,
        "setup": setup,
        "action": action,
        "setup_errors": [],
        "action_ok": False,
        "action_result_preview": None,
        "action_error": None,
        "post_errors": [],
    }

    try:
        inst.write("*CLS")
    except Exception as exc:
        record["setup_errors"].append(f"*CLS failed: {exc}")
        return record

    for command in setup:
        try:
            inst.write(command)
        except Exception as exc:
            record["setup_errors"].append(f"{command} failed: {exc}")
            record["post_errors"] = drain_errors(inst)
            return record

    try:
        if action["type"] == "write":
            inst.write(action["command"])
            record["action_ok"] = True
        elif action["type"] == "query":
            response = inst.query(action["command"])
            record["action_ok"] = True
            preview = response[:200]
            record["action_result_preview"] = preview
        else:
            record["action_error"] = f"Unsupported action type: {action['type']}"
    except Exception as exc:
        record["action_error"] = str(exc)

    record["post_errors"] = drain_errors(inst)
    return record


def run_single_command_case(inst: Any, command: str) -> Dict[str, Any]:
    record: Dict[str, Any] = {
        "command": command,
        "write_ok": False,
        "write_error": None,
        "post_errors": [],
    }
    try:
        inst.write("*CLS")
    except Exception as exc:
        record["write_error"] = f"*CLS failed: {exc}"
        return record

    try:
        inst.write(command)
        record["write_ok"] = True
    except Exception as exc:
        record["write_error"] = str(exc)

    record["post_errors"] = drain_errors(inst)
    return record


def main() -> int:
    parser = argparse.ArgumentParser(description="Probe FSH SCPI compatibility.")
    parser.add_argument("--visa-resource", required=True)
    parser.add_argument("--visa-backend", default=None)
    parser.add_argument("--timeout", type=float, default=10.0)
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

        base_setup = [
            "UNIT:POW DBM",
            "DISP:TRAC:Y:SPAC LOG",
            "DISP:TRAC:Y 80dB",
            "DISP:TRAC:Y:RLEV 0dBm",
            "INP:ATT:AUTO ON",
            "INP:GAIN:STAT OFF",
            "INP:IMP 50",
            "BAND:AUTO OFF",
            "BAND 10000",
            "BAND:VID:AUTO OFF",
            "BAND:VID 10000",
            "SWE:TIME:AUTO ON",
            "INIT:CONT OFF",
            "SWE:COUN 1",
            "DISP:WIND:TRAC:MODE AVER",
            "DET:AUTO OFF",
            "DET RMS",
            "FREQ:CENT 400000000",
            "FREQ:SPAN 1000000",
            "CALC:MARK1 ON",
            "CALC:MARK1:FREQ:MODE FREQ",
            "INIT;*WAI",
            "CALC:MARK1:MAX",
        ]

        tests = [
            ("form_asc", base_setup, {"type": "write", "command": "FORM ASC"}),
            ("form_real32", base_setup, {"type": "write", "command": "FORM REAL,32"}),
            ("form_data_asc", base_setup, {"type": "write", "command": "FORM:DATA ASC"}),
            ("form_data_real32", base_setup, {"type": "write", "command": "FORM:DATA REAL,32"}),
            ("form_border_swap", base_setup, {"type": "write", "command": "FORM:BORD SWAP"}),
            ("trace_data_trace1_ascii", base_setup + ["FORM ASC"], {"type": "query", "command": "TRAC:DATA? TRACE1"}),
            ("trace_alias_trace1_ascii", base_setup + ["FORM ASC"], {"type": "query", "command": "TRAC? TRACE1"}),
            ("trace_data_mem_trace1_ascii", base_setup + ["FORM ASC", "CALC:MATH:COPY:MEM", "DISP:TRAC:MEM ON"], {"type": "query", "command": "TRAC:DATA:MEM? TRACE1"}),
            ("trace_data_trace1_real32", base_setup + ["FORM REAL,32"], {"type": "query", "command": "TRAC:DATA? TRACE1"}),
            ("trace_alias_trace1_real32", base_setup + ["FORM REAL,32"], {"type": "query", "command": "TRAC? TRACE1"}),
            ("trace_data_mem_trace1_real32", base_setup + ["FORM REAL,32", "CALC:MATH:COPY:MEM", "DISP:TRAC:MEM ON"], {"type": "query", "command": "TRAC:DATA:MEM? TRACE1"}),
        ]

        single_command_candidates = [
            "UNIT:POW DBM",
            "DISP:TRAC:Y:SPAC LOG",
            "DISP:TRAC:Y 80dB",
            "DISP:TRAC:Y:RLEV 0dBm",
            "INP:ATT:AUTO ON",
            "INP:GAIN:STAT OFF",
            "INP:IMP 50",
            "BAND:AUTO OFF",
            "BAND 10000",
            "BAND:VID:AUTO OFF",
            "BAND:VID 10000",
            "SWE:TIME:AUTO ON",
            "INIT:CONT OFF",
            "SWE:COUN 1",
            "DISP:WIND:TRAC:MODE AVER",
            "DET:AUTO OFF",
            "DET RMS",
            "FREQ:CENT 400000000",
            "FREQ:SPAN 1000000",
            "CALC:MARK1 ON",
            "CALC:MARK1:FREQ:MODE FREQ",
            "CALC:MARK1:MAX",
            "FORM ASC",
            "FORM REAL,32",
            "FORM:DATA ASC",
            "FORM:DATA REAL,32",
            "FORM:BORD SWAP",
            "CALC:MATH:COPY:MEM",
            "DISP:TRAC:MEM ON",
        ]

        result = {
            "idn": inst.query("*IDN?").strip(),
            "visa_backend": args.visa_backend,
            "resource": args.visa_resource,
            "timeout_s": args.timeout,
            "single_command_tests": [run_single_command_case(inst, command) for command in single_command_candidates],
            "tests": [run_case(inst, name, setup, action) for name, setup, action in tests],
        }
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
