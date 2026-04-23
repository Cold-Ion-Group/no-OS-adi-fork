#!/usr/bin/env python3
"""
Capture FMCDAC boot-time Subclass 1 signatures across repeated boots or power cycles.

This helper is intentionally separate from the FSH8 benchmark flow. It only cares
about the UART-visible boot evidence needed for deterministic-latency and SYSREF
repeatability:

1. SYSREF tune outcome
2. TX SYSREF_STATUS readback
3. AD9144 latency signature (dyn0/dyn1/var0/var1)

The script waits until the firmware reaches its first paused diagnostic prompt,
then parses the boot log and writes per-cycle plus aggregate artifacts.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import List, Optional

from run_nco_scope_test import (
    BOOT_CHOICE_PROMPT,
    BOOT_RATE_PROMPT,
    CLOCK_CONFIG_PROMPT,
    CONTINUE_PROMPT,
    DDS_BAND_START_PROMPT,
    DDS_SWEEP_START_MARKER,
    NCO_START_PROMPT,
    SFDR_START_PROMPT,
    THROUGHPUT_START_PROMPT,
    UART_RTT_START_PROMPT,
    ConsoleLog,
    UartCoordinator,
    resolve_xilinx_settings,
    run_make_run,
    utc_timestamp,
)


LATENCY_RE = re.compile(
    r"\[LATENCY\]\s+dyn0=0x([0-9A-Fa-f]{2})\s+dyn1=0x([0-9A-Fa-f]{2})\s+"
    r"var0=0x([0-9A-Fa-f]{2})\s+var1=0x([0-9A-Fa-f]{2})"
)
LATENCY_PRE_RE = re.compile(
    r"\[LATENCY-PRE\]\s+dyn0=0x([0-9A-Fa-f]{2})\s+dyn1=0x([0-9A-Fa-f]{2})\s+"
    r"var0=0x([0-9A-Fa-f]{2})\s+var1=0x([0-9A-Fa-f]{2})"
)
SYSREF_STATUS_RE = re.compile(
    r"\[SYSREF-VERIFY\]\s+TX SYSREF_STATUS \(0x108\)\s+=\s+0x([0-9A-Fa-f]+)"
)
SYSREF_PRE_STATUS_RE = re.compile(
    r"\[SYSREF-PRE\]\s+TX SYSREF_STATUS \(0x108\)\s+=\s+0x([0-9A-Fa-f]+)"
)
SYSREF_ACTRL0_RE = re.compile(
    r"\[SYSREF-VERIFY\]\s+SYSREF_ACTRL0 \(0x081\)\s+=\s+0x([0-9A-Fa-f]{2})"
)
SYSREF_PRE_ACTRL0_RE = re.compile(
    r"\[SYSREF-PRE\]\s+SYSREF_ACTRL0 \(0x081\)\s+=\s+0x([0-9A-Fa-f]{2})"
)


@dataclass
class BootCycleSummary:
    cycle: int
    reached_marker: str
    sysref_tune: str
    pre_tune_sysref_status_hex: Optional[str]
    pre_tune_sysref_status_captured: Optional[bool]
    pre_tune_sysref_status_alignment_error: Optional[bool]
    pre_tune_sysref_actrl0_hex: Optional[str]
    pre_tune_latency_dyn0_hex: Optional[str]
    pre_tune_latency_dyn1_hex: Optional[str]
    pre_tune_latency_var0_hex: Optional[str]
    pre_tune_latency_var1_hex: Optional[str]
    pre_tune_latency_signature: Optional[str]
    sysref_status_hex: Optional[str]
    sysref_status_captured: Optional[bool]
    sysref_status_alignment_error: Optional[bool]
    sysref_actrl0_hex: Optional[str]
    latency_dyn0_hex: Optional[str]
    latency_dyn1_hex: Optional[str]
    latency_var0_hex: Optional[str]
    latency_var1_hex: Optional[str]
    latency_signature: Optional[str]
    warnings: List[str]
    uart_log: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Capture FMCDAC SYSREF and latency boot signatures across repeated boots."
    )
    parser.add_argument("--serial-port", required=True, help="UART COM port, for example COM4")
    parser.add_argument("--baudrate", type=int, default=115200, help="UART baud rate")
    parser.add_argument(
        "--uart-timeout",
        type=float,
        default=120.0,
        help="Seconds to wait for each boot to reach the first paused prompt",
    )
    parser.add_argument(
        "--cycles",
        type=int,
        default=5,
        help="Number of boot captures to collect",
    )
    parser.add_argument(
        "--skip-make-run",
        action="store_true",
        help="Assume the board/firmware boots on its own and do not launch 'make run'",
    )
    parser.add_argument(
        "--make-timeout",
        type=float,
        default=300.0,
        help="Timeout for 'make run' in seconds",
    )
    parser.add_argument(
        "--make-args",
        default="",
        help="Optional extra arguments appended to 'make run'",
    )
    parser.add_argument(
        "--xilinx-settings",
        action="append",
        default=[],
        help="Optional settings64.bat path(s) to call before 'make run'. Repeat as needed.",
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
        "--output-dir",
        default=None,
        help="Directory for artifacts. Default creates capture_runs/boot_repeatability_<timestamp>.",
    )
    parser.add_argument(
        "--serial-dtr",
        choices=["on", "off", "leave"],
        default="leave",
        help="Initial UART DTR state",
    )
    parser.add_argument(
        "--serial-rts",
        choices=["on", "off", "leave"],
        default="leave",
        help="Initial UART RTS state",
    )
    parser.add_argument(
        "--no-prompt",
        action="store_true",
        help="Do not pause for Enter between cycles",
    )
    return parser.parse_args()


def normalize_line_state(value: str) -> Optional[bool]:
    if value == "on":
        return True
    if value == "off":
        return False
    return None


def project_dir() -> Path:
    return Path(__file__).resolve().parent


def default_output_dir() -> Path:
    return project_dir() / "capture_runs" / f"boot_repeatability_{utc_timestamp()}"


def advance_boot_menus(
    uart: UartCoordinator,
    clock_reply: str,
    rate_reply: str,
    timeout_s: float,
) -> str:
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
                DDS_BAND_START_PROMPT,
                SFDR_START_PROMPT,
                THROUGHPUT_START_PROMPT,
                UART_RTT_START_PROMPT,
                DDS_SWEEP_START_MARKER,
            ],
        )
        if seen == CLOCK_CONFIG_PROMPT:
            uart.wait_for(BOOT_CHOICE_PROMPT, timeout_s)
            uart.send_line(clock_reply)
            stage = "rate"
            continue
        if seen == BOOT_RATE_PROMPT:
            uart.wait_for(BOOT_CHOICE_PROMPT, timeout_s)
            uart.send_line(rate_reply)
            stage = "done"
            continue
        if seen == BOOT_CHOICE_PROMPT:
            uart.send_line(clock_reply if stage == "clock" else rate_reply)
            stage = "rate" if stage == "clock" else "done"
            continue
        return seen

    raise TimeoutError("Timed out waiting for the firmware boot sequence to reach a paused prompt")


def parse_cycle_log(text: str, cycle: int, reached_marker: str, uart_log: Path) -> BootCycleSummary:
    warnings: List[str] = []

    if "[SYSREF-TUNE] No alignment error - skipping tune" in text:
        sysref_tune = "clean_skip"
    elif "[SYSREF-TUNE] FIXED:" in text:
        sysref_tune = "fixed_by_tune"
    elif "[SYSREF-TUNE] FAILED:" in text:
        sysref_tune = "failed"
    else:
        sysref_tune = "missing"
        warnings.append("SYSREF tune result not found in boot log")

    def parse_status(match: Optional[re.Match[str]]) -> tuple[Optional[str], Optional[bool], Optional[bool]]:
        if not match:
            return None, None, None
        raw = int(match.group(1), 16)
        return f"0x{raw:08X}", bool(raw & 0x1), bool(raw & 0x2)

    def parse_hex_byte(match: Optional[re.Match[str]]) -> Optional[str]:
        if not match:
            return None
        return f"0x{int(match.group(1), 16):02X}"

    def parse_latency(match: Optional[re.Match[str]]) -> tuple[Optional[str], Optional[str], Optional[str], Optional[str], Optional[str]]:
        if not match:
            return None, None, None, None, None
        dyn0_hex, dyn1_hex, var0_hex, var1_hex = [
            f"0x{int(item, 16):02X}" for item in match.groups()
        ]
        return (
            dyn0_hex,
            dyn1_hex,
            var0_hex,
            var1_hex,
            f"{dyn0_hex}/{dyn1_hex}/{var0_hex}/{var1_hex}",
        )

    (
        pre_tune_sysref_status_hex,
        pre_tune_sysref_status_captured,
        pre_tune_sysref_status_alignment_error,
    ) = parse_status(SYSREF_PRE_STATUS_RE.search(text))
    if pre_tune_sysref_status_hex is None:
        warnings.append("Pre-tune SYSREF_STATUS readback not found in boot log")

    pre_tune_sysref_actrl0_hex = parse_hex_byte(SYSREF_PRE_ACTRL0_RE.search(text))
    (
        pre_tune_dyn0,
        pre_tune_dyn1,
        pre_tune_var0,
        pre_tune_var1,
        pre_tune_latency_signature,
    ) = parse_latency(LATENCY_PRE_RE.search(text))
    if pre_tune_latency_signature is None:
        warnings.append("Pre-tune latency signature not found in boot log")

    (
        sysref_status_hex,
        sysref_status_captured,
        sysref_status_alignment_error,
    ) = parse_status(SYSREF_STATUS_RE.search(text))
    if sysref_status_hex is None:
        warnings.append("SYSREF_STATUS readback not found in boot log")

    sysref_actrl0_hex = parse_hex_byte(SYSREF_ACTRL0_RE.search(text))
    dyn0, dyn1, var0, var1, latency_signature = parse_latency(LATENCY_RE.search(text))
    if latency_signature is None:
        warnings.append("Latency signature not found in boot log")

    if "[WARN] TX SYSREF alignment error detected" in text:
        warnings.append("Firmware reported TX SYSREF alignment error")
    if pre_tune_sysref_status_alignment_error:
        warnings.append("Pre-tune TX SYSREF_STATUS alignment_error bit is set")
    if sysref_status_alignment_error:
        warnings.append("TX SYSREF_STATUS alignment_error bit is set")
    if sysref_tune == "fixed_by_tune":
        warnings.append("Boot required SYSREF tune to reach a clean state")
    if sysref_tune == "failed":
        warnings.append("SYSREF tune failed to clear alignment")
    if pre_tune_sysref_status_captured is False:
        warnings.append("Pre-tune TX SYSREF_STATUS captured bit is not set")
    if sysref_status_captured is False:
        warnings.append("TX SYSREF_STATUS captured bit is not set")

    return BootCycleSummary(
        cycle=cycle,
        reached_marker=reached_marker,
        sysref_tune=sysref_tune,
        pre_tune_sysref_status_hex=pre_tune_sysref_status_hex,
        pre_tune_sysref_status_captured=pre_tune_sysref_status_captured,
        pre_tune_sysref_status_alignment_error=pre_tune_sysref_status_alignment_error,
        pre_tune_sysref_actrl0_hex=pre_tune_sysref_actrl0_hex,
        pre_tune_latency_dyn0_hex=pre_tune_dyn0,
        pre_tune_latency_dyn1_hex=pre_tune_dyn1,
        pre_tune_latency_var0_hex=pre_tune_var0,
        pre_tune_latency_var1_hex=pre_tune_var1,
        pre_tune_latency_signature=pre_tune_latency_signature,
        sysref_status_hex=sysref_status_hex,
        sysref_status_captured=sysref_status_captured,
        sysref_status_alignment_error=sysref_status_alignment_error,
        sysref_actrl0_hex=sysref_actrl0_hex,
        latency_dyn0_hex=dyn0,
        latency_dyn1_hex=dyn1,
        latency_var0_hex=var0,
        latency_var1_hex=var1,
        latency_signature=latency_signature,
        warnings=warnings,
        uart_log=str(uart_log.resolve()),
    )


def write_cycle_csv(path: Path, cycles: List[BootCycleSummary]) -> None:
    with path.open("w", encoding="utf-8", newline="") as fp:
        writer = csv.writer(fp)
        writer.writerow(
            [
                "cycle",
                "reached_marker",
                "sysref_tune",
                "pre_tune_sysref_status_hex",
                "pre_tune_sysref_status_captured",
                "pre_tune_sysref_status_alignment_error",
                "pre_tune_sysref_actrl0_hex",
                "pre_tune_latency_dyn0_hex",
                "pre_tune_latency_dyn1_hex",
                "pre_tune_latency_var0_hex",
                "pre_tune_latency_var1_hex",
                "pre_tune_latency_signature",
                "sysref_status_hex",
                "sysref_status_captured",
                "sysref_status_alignment_error",
                "sysref_actrl0_hex",
                "latency_dyn0_hex",
                "latency_dyn1_hex",
                "latency_var0_hex",
                "latency_var1_hex",
                "latency_signature",
                "warnings",
                "uart_log",
            ]
        )
        for item in cycles:
            writer.writerow(
                [
                    item.cycle,
                    item.reached_marker,
                    item.sysref_tune,
                    item.pre_tune_sysref_status_hex,
                    item.pre_tune_sysref_status_captured,
                    item.pre_tune_sysref_status_alignment_error,
                    item.pre_tune_sysref_actrl0_hex,
                    item.pre_tune_latency_dyn0_hex,
                    item.pre_tune_latency_dyn1_hex,
                    item.pre_tune_latency_var0_hex,
                    item.pre_tune_latency_var1_hex,
                    item.pre_tune_latency_signature,
                    item.sysref_status_hex,
                    item.sysref_status_captured,
                    item.sysref_status_alignment_error,
                    item.sysref_actrl0_hex,
                    item.latency_dyn0_hex,
                    item.latency_dyn1_hex,
                    item.latency_var0_hex,
                    item.latency_var1_hex,
                    item.latency_signature,
                    "; ".join(item.warnings),
                    item.uart_log,
                ]
            )


def build_aggregate_summary(args: argparse.Namespace, cycles: List[BootCycleSummary]) -> dict:
    signatures = sorted({item.latency_signature for item in cycles if item.latency_signature})
    pre_tune_signatures = sorted(
        {item.pre_tune_latency_signature for item in cycles if item.pre_tune_latency_signature}
    )
    clean_cycles = [
        item.cycle
        for item in cycles
        if item.sysref_tune == "clean_skip"
        and item.sysref_status_alignment_error is False
        and item.latency_signature is not None
    ]
    clean_init_cycles = [
        item.cycle
        for item in cycles
        if item.pre_tune_sysref_status_captured is True
        and item.pre_tune_sysref_status_alignment_error is False
        and item.pre_tune_latency_signature is not None
    ]
    failing_cycles = [item.cycle for item in cycles if item.warnings]
    tuned_cycles = [item.cycle for item in cycles if item.sysref_tune == "fixed_by_tune"]
    alignment_error_cycles = [
        item.cycle for item in cycles if item.sysref_status_alignment_error is True
    ]
    pre_tune_alignment_error_cycles = [
        item.cycle for item in cycles if item.pre_tune_sysref_status_alignment_error is True
    ]
    missing_latency_cycles = [item.cycle for item in cycles if item.latency_signature is None]
    missing_pre_tune_latency_cycles = [
        item.cycle for item in cycles if item.pre_tune_latency_signature is None
    ]

    review_reasons: List[str] = []
    if len(cycles) != args.cycles:
        review_reasons.append(
            f"Captured {len(cycles)} cycles but requested {args.cycles}"
        )
    if tuned_cycles:
        review_reasons.append(
            f"SYSREF tune was required on cycles {', '.join(str(item) for item in tuned_cycles)}"
        )
    if alignment_error_cycles:
        review_reasons.append(
            "Alignment error remained set on cycles "
            + ", ".join(str(item) for item in alignment_error_cycles)
        )
    if pre_tune_alignment_error_cycles:
        review_reasons.append(
            "Pre-tune alignment error was present on cycles "
            + ", ".join(str(item) for item in pre_tune_alignment_error_cycles)
        )
    if len(signatures) > 1:
        review_reasons.append(
            "Multiple latency signatures observed: " + ", ".join(signatures)
        )
    if len(pre_tune_signatures) > 1:
        review_reasons.append(
            "Multiple pre-tune latency signatures observed: "
            + ", ".join(pre_tune_signatures)
        )
    if missing_latency_cycles:
        review_reasons.append(
            "Latency signature missing on cycles "
            + ", ".join(str(item) for item in missing_latency_cycles)
        )
    if missing_pre_tune_latency_cycles:
        review_reasons.append(
            "Pre-tune latency signature missing on cycles "
            + ", ".join(str(item) for item in missing_pre_tune_latency_cycles)
        )

    if len(cycles) == 0:
        verdict = "REVIEW"
    elif len(review_reasons) == 0:
        verdict = "PASS"
    elif (
        len(tuned_cycles) == len(cycles)
        and not alignment_error_cycles
        and len(signatures) == 1
        and not missing_latency_cycles
    ):
        verdict = "STABLE_AFTER_TUNE"
    else:
        verdict = "REVIEW"

    if (
        len(cycles) > 0
        and len(clean_init_cycles) == len(cycles)
        and len(pre_tune_signatures) == 1
        and not missing_pre_tune_latency_cycles
    ):
        clean_init_verdict = "PASS"
    else:
        clean_init_verdict = "REVIEW"

    return {
        "timestamp_utc": utc_timestamp(),
        "serial_port": args.serial_port,
        "cycles_requested": args.cycles,
        "cycles_captured": len(cycles),
        "verdict": verdict,
        "clean_init_verdict": clean_init_verdict,
        "review_reasons": review_reasons,
        "tuned_cycles": tuned_cycles,
        "pre_tune_alignment_error_cycles": pre_tune_alignment_error_cycles,
        "alignment_error_cycles": alignment_error_cycles,
        "pre_tune_latency_signatures": pre_tune_signatures,
        "pre_tune_latency_signature_count": len(pre_tune_signatures),
        "latency_signatures": signatures,
        "latency_signature_count": len(signatures),
        "all_cycles_clean_skip": len(clean_cycles) == len(cycles),
        "all_cycles_clean_from_init": len(clean_init_cycles) == len(cycles),
        "all_cycles_same_pre_tune_latency_signature": (
            len(pre_tune_signatures) == 1 and len(cycles) > 0
        ),
        "all_cycles_same_latency_signature": len(signatures) == 1 and len(cycles) > 0,
        "clean_cycles": clean_cycles,
        "clean_init_cycles": clean_init_cycles,
        "cycles_with_warnings": failing_cycles,
        "cycles": [asdict(item) for item in cycles],
    }


def wait_for_user_cycle(cycle: int, total: int) -> None:
    prompt = (
        f"[HOST] Cycle {cycle}/{total}: power cycle or restart the board, "
        "then press Enter to begin capture..."
    )
    input(prompt)


def main() -> int:
    args = parse_args()
    if args.cycles < 1:
        print("--cycles must be at least 1", file=sys.stderr)
        return 2

    out_dir = Path(args.output_dir) if args.output_dir else default_output_dir()
    out_dir.mkdir(parents=True, exist_ok=True)

    dtr = normalize_line_state(args.serial_dtr)
    rts = normalize_line_state(args.serial_rts)
    settings_files = resolve_xilinx_settings(args)
    cycles: List[BootCycleSummary] = []

    uart: Optional[UartCoordinator] = None
    main_log = ConsoleLog(out_dir / "session_uart.log")

    try:
        uart = UartCoordinator(
            port=args.serial_port,
            baudrate=args.baudrate,
            timeout_s=args.uart_timeout,
            log=main_log,
            dtr=dtr,
            rts=rts,
        )

        for cycle in range(1, args.cycles + 1):
            if not args.no_prompt:
                wait_for_user_cycle(cycle, args.cycles)

            cycle_log_path = out_dir / f"cycle{cycle:02d}_uart.log"
            cycle_log = ConsoleLog(cycle_log_path)
            uart.log = cycle_log
            uart._buffer = ""
            reached: Optional[str] = None

            try:
                if cycle > 1:
                    uart.reopen()

                if args.skip_make_run:
                    reached = advance_boot_menus(
                        uart=uart,
                        clock_reply=args.clock_choice,
                        rate_reply=args.rate_choice,
                        timeout_s=args.uart_timeout,
                    )
                else:
                    run_make_run(
                        project_dir(),
                        uart,
                        args.make_timeout,
                        settings_files,
                        args.make_args,
                    )
                    reached = advance_boot_menus(
                        uart=uart,
                        clock_reply=args.clock_choice,
                        rate_reply=args.rate_choice,
                        timeout_s=args.uart_timeout,
                    )
            finally:
                cycle_log.close()
                uart.log = main_log

            if reached is None:
                raise RuntimeError(f"Cycle {cycle} did not reach a paused prompt")

            summary = parse_cycle_log(
                text=cycle_log_path.read_text(encoding="utf-8", errors="replace"),
                cycle=cycle,
                reached_marker=reached,
                uart_log=cycle_log_path,
            )
            cycles.append(summary)
            print(
                f"[HOST] cycle={cycle} tune={summary.sysref_tune} "
                f"pre_status={summary.pre_tune_sysref_status_hex or 'missing'} "
                f"pre_latency={summary.pre_tune_latency_signature or 'missing'} "
                f"sysref_status={summary.sysref_status_hex} "
                f"latency={summary.latency_signature or 'missing'}"
            )

        aggregate = build_aggregate_summary(args, cycles)
        (out_dir / "boot_repeatability.json").write_text(
            json.dumps(aggregate, indent=2) + "\n",
            encoding="utf-8",
        )
        write_cycle_csv(out_dir / "boot_repeatability.csv", cycles)

        verdict = aggregate["verdict"]
        print(
            f"[HOST] {verdict} "
            f"(clean_init={aggregate['clean_init_verdict']}): artifacts written to {out_dir}"
        )
        for reason in aggregate["review_reasons"]:
            print(f"[HOST] reason: {reason}")
        return 0
    except Exception as exc:
        print(f"[HOST] ERROR: {exc}", file=sys.stderr)
        return 1
    finally:
        if uart is not None:
            try:
                uart.close()
            except Exception:
                pass
        main_log.close()


if __name__ == "__main__":
    raise SystemExit(main())
