#!/usr/bin/env python3
"""
Minimal AWG scheduler sweep test for FMCDAC.

This uses the existing UART helpers in run_nco_scope_test.py but avoids the
analyzer dependency by driving only the prompt flow.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from types import SimpleNamespace

from run_nco_scope_test import (
    AWG_SWEEP_START_PROMPT,
    DDS_BAND_START_PROMPT,
    DDS_SWEEP_START_MARKER,
    DYNAMIC_SFDR_START_PROMPT,
    NCO_START_PROMPT,
    SFDR_START_PROMPT,
    THROUGHPUT_START_PROMPT,
    UART_RTT_START_PROMPT,
    ConsoleLog,
    UartCoordinator,
    advance_boot_defaults_or_wait_for_nco,
    build_sweep_override_cflags,
    combine_make_args,
    resolve_xilinx_settings,
    run_make_run,
    utc_timestamp,
    wait_for_optional_prompt,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Minimal AWG scheduler sweep test runner."
    )
    parser.add_argument("--serial-port", required=True, help="UART COM port, e.g. COM4")
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
        help="Skip the boot menus and wait directly for the NCO prompt.",
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
        default=1000,
        help="AWG sweep dwell per step in microseconds.",
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
        default=1000,
        help="AWG sweep start tick offset.",
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


def build_awg_cflags(args: argparse.Namespace) -> str:
    shim = SimpleNamespace(
        dds_band_sweep_start_hz=None,
        dds_band_sweep_stop_hz=None,
        dds_band_sweep_step_hz=None,
        sfdr_sweep_start_hz=None,
        sfdr_sweep_stop_hz=None,
        sfdr_sweep_step_hz=None,
        awg_sweep_start_hz=args.awg_sweep_start_hz,
        awg_sweep_stop_hz=args.awg_sweep_stop_hz,
        awg_sweep_step_hz=args.awg_sweep_step_hz,
        awg_sweep_dwell_us=args.awg_sweep_dwell_us,
        awg_sweep_scale_u=args.awg_sweep_scale_u,
        awg_sweep_start_ticks=args.awg_sweep_start_ticks,
        awg_sweep_tone=args.awg_sweep_tone,
        awg_sched_baseaddr=args.awg_sched_baseaddr,
        awg_sched_max_events=args.awg_sched_max_events,
        awg_sched_tick_hz=args.awg_sched_tick_hz,
        awg_sched_timeout_ms=args.awg_sched_timeout_ms,
        run_awg_sweep=True,
    )
    return build_sweep_override_cflags(shim, benchmark_prompts_disabled=False)


def main() -> int:
    args = parse_args()
    script_dir = Path(__file__).resolve().parent
    output_dir = (
        Path(args.output_dir)
        if args.output_dir
        else script_dir / "capture_runs" / f"awg_sweep_{utc_timestamp()}"
    )
    output_dir.mkdir(parents=True, exist_ok=True)

    console_log = ConsoleLog(output_dir / "uart.log")
    uart = None
    try:
        uart = UartCoordinator(
            port=args.serial_port,
            baudrate=args.baudrate,
            timeout_s=args.uart_timeout,
            log=console_log,
            dtr=None,
            rts=None,
        )
        print(f"[AWG-TEST] UART connected: {args.serial_port} @ {args.baudrate}")

        settings_files = resolve_xilinx_settings(args)
        extra_cflags = build_awg_cflags(args)
        make_env = {"NEW_CFLAGS": extra_cflags} if extra_cflags else None
        make_args = combine_make_args(args.make_args)
        force_rebuild = bool(extra_cflags)

        if not args.skip_make_run:
            for item in settings_files:
                print(f"[AWG-TEST] Using Xilinx settings: {item}")
            print("[AWG-TEST] Launching 'make run'...")
            run_make_run(
                project_dir=script_dir,
                uart=uart,
                timeout_s=args.make_timeout,
                settings_files=settings_files,
                make_args=make_args,
                extra_env=make_env,
                update_first=force_rebuild,
                clean_first=force_rebuild,
            )
            print("[AWG-TEST] 'make run' completed.")

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

        uart.send_line("n")
        print("[AWG-TEST] NCO discriminator skipped.")

        next_prompt = wait_for_optional_prompt(
            uart,
            AWG_SWEEP_START_PROMPT,
            args.uart_timeout,
            extra_needles=[
                DDS_BAND_START_PROMPT,
                SFDR_START_PROMPT,
                DYNAMIC_SFDR_START_PROMPT,
                THROUGHPUT_START_PROMPT,
                UART_RTT_START_PROMPT,
                DDS_SWEEP_START_MARKER,
            ],
        )
        if next_prompt != AWG_SWEEP_START_PROMPT:
            raise RuntimeError("AWG sweep prompt was not reached; check firmware build.")

        uart.send_line("y")
        print("[AWG-TEST] AWG scheduler sweep started.")

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
            uart.send_line("n")
            print("[AWG-TEST] DDS-band diagnostic skipped.")

        print(f"[AWG-TEST] Done. UART log: {output_dir / 'uart.log'}")
        return 0
    except Exception as exc:
        print(f"[AWG-TEST] ERROR: {exc}")
        return 1
    finally:
        if uart is not None:
            try:
                uart.close()
            except Exception:
                pass
        console_log.close()


if __name__ == "__main__":
    raise SystemExit(main())
