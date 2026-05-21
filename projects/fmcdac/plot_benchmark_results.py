#!/usr/bin/env python3
"""Render simple SVG plots from FMCDAC benchmark CSV outputs."""

from __future__ import annotations

import argparse
import csv
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable, List, Optional, Sequence, Tuple


Color = str


def escape_xml(text: str) -> str:
    return (
        text.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
    )


def read_csv(path: Path) -> List[dict]:
    with Path(path).open("r", encoding="utf-8", newline="") as fp:
        return list(csv.DictReader(fp))


def fvalue(row: dict, key: str) -> Optional[float]:
    value = row.get(key, "")
    if value == "":
        return None
    return float(value)


def nice_floor(value: float, step: float) -> float:
    return math.floor(value / step) * step


def nice_ceil(value: float, step: float) -> float:
    return math.ceil(value / step) * step


@dataclass(frozen=True)
class Series:
    label: str
    points: List[Tuple[float, float, str]]
    color: Color


def write_xy_svg(
    output_path: Path,
    *,
    title: str,
    subtitle: str,
    x_label: str,
    y_label: str,
    series_list: Sequence[Series],
    x_tick_labels: Optional[Callable[[float], str]] = None,
    y_unit: str = "",
    y_step: float = 5.0,
) -> None:
    all_points = [point for series in series_list for point in series.points]
    if not all_points:
        raise ValueError(f"No points to plot for {output_path}")

    x_values = [point[0] for point in all_points]
    y_values = [point[1] for point in all_points]
    x_min = min(x_values)
    x_max = max(x_values)
    if x_min == x_max:
        x_min -= 0.5
        x_max += 0.5
    y_min = min(y_values)
    y_max = max(y_values)
    y_bottom = nice_floor(y_min, y_step)
    y_top = nice_ceil(y_max, y_step)
    if y_bottom == y_top:
        y_bottom -= y_step
        y_top += y_step

    width = 1120
    height = 700
    left = 100
    right = 50
    top = 86
    bottom = 120
    plot_w = width - left - right
    plot_h = height - top - bottom

    def sx(value: float) -> float:
        return left + ((value - x_min) / (x_max - x_min)) * plot_w

    def sy(value: float) -> float:
        return top + ((y_top - value) / (y_top - y_bottom)) * plot_h

    if x_tick_labels is None:
        x_tick_labels = lambda value: f"{value:g}"

    svg: List[str] = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#f8f9fa"/>',
        f'<text x="{width / 2:.1f}" y="34" text-anchor="middle" font-size="24" font-family="Segoe UI, Arial, sans-serif" fill="#212529">{escape_xml(title)}</text>',
        f'<text x="{width / 2:.1f}" y="58" text-anchor="middle" font-size="13" font-family="Segoe UI, Arial, sans-serif" fill="#495057">{escape_xml(subtitle)}</text>',
        f'<rect x="{left}" y="{top}" width="{plot_w}" height="{plot_h}" fill="#ffffff" stroke="#adb5bd" stroke-width="1"/>',
    ]

    y_tick = y_bottom
    while y_tick <= y_top + 1e-9:
        y = sy(y_tick)
        svg.append(f'<line x1="{left}" y1="{y:.2f}" x2="{left + plot_w}" y2="{y:.2f}" stroke="#e9ecef" stroke-width="1"/>')
        svg.append(f'<text x="{left - 12}" y="{y + 4:.2f}" text-anchor="end" font-size="12" font-family="Segoe UI, Arial, sans-serif" fill="#495057">{y_tick:g}{escape_xml(y_unit)}</text>')
        y_tick += y_step

    x_ticks = sorted(set(x_values))
    if len(x_ticks) > 12:
        stride = max(1, math.ceil(len(x_ticks) / 12))
        x_ticks = x_ticks[::stride]
        if x_max not in x_ticks:
            x_ticks.append(x_max)
    for x_tick in x_ticks:
        x = sx(x_tick)
        svg.append(f'<line x1="{x:.2f}" y1="{top}" x2="{x:.2f}" y2="{top + plot_h}" stroke="#f1f3f5" stroke-width="1"/>')
        svg.append(f'<text x="{x:.2f}" y="{top + plot_h + 26}" text-anchor="middle" font-size="12" font-family="Segoe UI, Arial, sans-serif" fill="#495057">{escape_xml(x_tick_labels(x_tick))}</text>')

    svg.append(f'<text x="{left + plot_w / 2:.2f}" y="{height - 36}" text-anchor="middle" font-size="14" font-family="Segoe UI, Arial, sans-serif" fill="#212529">{escape_xml(x_label)}</text>')
    svg.append(f'<text x="28" y="{top + plot_h / 2:.2f}" text-anchor="middle" font-size="14" font-family="Segoe UI, Arial, sans-serif" fill="#212529" transform="rotate(-90 28 {top + plot_h / 2:.2f})">{escape_xml(y_label)}</text>')

    legend_x = left + 12
    legend_y = 78
    for index, series in enumerate(series_list):
        y = legend_y + index * 20
        svg.append(f'<line x1="{legend_x}" y1="{y}" x2="{legend_x + 28}" y2="{y}" stroke="{series.color}" stroke-width="3"/>')
        svg.append(f'<circle cx="{legend_x + 14}" cy="{y}" r="4" fill="{series.color}"/>')
        svg.append(f'<text x="{legend_x + 38}" y="{y + 4}" font-size="13" font-family="Segoe UI, Arial, sans-serif" fill="#212529">{escape_xml(series.label)}</text>')

    for series in series_list:
        coords = [(sx(x), sy(y), tooltip) for x, y, tooltip in series.points]
        if len(coords) > 1:
            svg.append(
                f'<polyline fill="none" stroke="{series.color}" stroke-width="2.5" points="'
                + " ".join(f"{x:.2f},{y:.2f}" for x, y, _ in coords)
                + '"/>'
            )
        for x, y, tooltip in coords:
            svg.append(
                f'<g><title>{escape_xml(tooltip)}</title>'
                f'<circle cx="{x:.2f}" cy="{y:.2f}" r="5" fill="{series.color}" stroke="#ffffff" stroke-width="1.5"/></g>'
            )

    svg.append("</svg>")
    Path(output_path).write_text("\n".join(svg) + "\n", encoding="utf-8")


def dynamic_case_label(name: str) -> str:
    return name.replace("dynamic_toggle_", "").replace("_", " ")


def render_dynamic(csv_path: Path, output_dir: Path) -> List[Path]:
    rows = read_csv(csv_path)
    x_values = list(range(1, len(rows) + 1))
    labels = {float(index): dynamic_case_label(row["name"]) for index, row in zip(x_values, rows)}

    def label_for(value: float) -> str:
        return labels.get(value, f"{value:g}")

    power_series = [
        Series(
            "Intended carrier 1",
            [
                (float(index), fvalue(row, "intended1_power_dbm"), f'{row["name"]}: {row["intended1_label"]} {row["intended1_power_dbm"]} dBm')
                for index, row in zip(x_values, rows)
                if fvalue(row, "intended1_power_dbm") is not None
            ],
            "#0b7285",
        ),
        Series(
            "Intended carrier 2",
            [
                (float(index), fvalue(row, "intended2_power_dbm"), f'{row["name"]}: {row["intended2_label"]} {row["intended2_power_dbm"]} dBm')
                for index, row in zip(x_values, rows)
                if fvalue(row, "intended2_power_dbm") is not None
            ],
            "#2b8a3e",
        ),
        Series(
            "Strongest spur",
            [
                (float(index), fvalue(row, "spur_power_dbm"), f'{row["name"]}: spur {row["spur_power_dbm"]} dBm @ {row["spur_freq_mhz"]} MHz')
                for index, row in zip(x_values, rows)
                if fvalue(row, "spur_power_dbm") is not None
            ],
            "#c92a2a",
        ),
    ]
    power_svg = output_dir / "dynamic_sfdr_power.svg"
    write_xy_svg(
        power_svg,
        title="Dynamic Retune Intended Carriers vs Spur",
        subtitle="Marker-locked intended carrier powers and strongest out-of-band peak",
        x_label="Dynamic case",
        y_label="Power (dBm)",
        series_list=power_series,
        x_tick_labels=label_for,
        y_unit=" dBm",
        y_step=5.0,
    )

    timing_svg = output_dir / "dynamic_sfdr_timing.svg"
    write_xy_svg(
        timing_svg,
        title="Dynamic Retune Measured Timing",
        subtitle="Firmware-reported elapsed time divided by transition count",
        x_label="Dynamic case",
        y_label="Measured time per transition (us)",
        series_list=[
            Series(
                "Measured us/transition",
                [
                    (float(index), fvalue(row, "measured_us_per_transition"), f'{row["name"]}: {row["measured_us_per_transition"]} us/transition')
                    for index, row in zip(x_values, rows)
                    if fvalue(row, "measured_us_per_transition") is not None
                ],
                "#5f3dc4",
            )
        ],
        x_tick_labels=label_for,
        y_unit=" us",
        y_step=2000.0,
    )
    return [power_svg, timing_svg]


def render_phase_noise(csv_path: Path, output_dir: Path) -> List[Path]:
    rows = read_csv(csv_path)
    offsets = sorted({fvalue(row, "offset_hz") for row in rows if fvalue(row, "offset_hz") is not None})
    colors = ["#0b7285", "#c92a2a", "#2b8a3e", "#e67700"]
    series_list: List[Series] = []
    for index, offset in enumerate(offsets):
        points = []
        for row in rows:
            if fvalue(row, "offset_hz") != offset:
                continue
            carrier_mhz = fvalue(row, "carrier_mhz")
            dbc_hz = fvalue(row, "avg_sideband_dbc_per_hz")
            if carrier_mhz is None or dbc_hz is None:
                continue
            points.append(
                (
                    carrier_mhz,
                    dbc_hz,
                    f'{carrier_mhz:.0f} MHz, offset {offset:.0f} Hz: {dbc_hz:.2f} dBc/Hz',
                )
            )
        series_list.append(Series(f"{offset / 1000:g} kHz offset", points, colors[index % len(colors)]))

    output_svg = output_dir / "phase_noise_offset_plot.svg"
    write_xy_svg(
        output_svg,
        title="Marker-Only Phase-Noise Offset Measurement",
        subtitle="Average sideband density from left/right marker offsets",
        x_label="Carrier (MHz)",
        y_label="Average sideband density (dBc/Hz)",
        series_list=series_list,
        x_tick_labels=lambda value: f"{value:.0f}",
        y_unit="",
        y_step=1.0,
    )
    return [output_svg]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dynamic-csv", type=Path)
    parser.add_argument("--phase-noise-csv", type=Path)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()

    outputs: List[Path] = []
    if args.dynamic_csv:
        output_dir = args.output_dir or args.dynamic_csv.parent
        output_dir.mkdir(parents=True, exist_ok=True)
        outputs.extend(render_dynamic(args.dynamic_csv, output_dir))
    if args.phase_noise_csv:
        output_dir = args.output_dir or args.phase_noise_csv.parent
        output_dir.mkdir(parents=True, exist_ok=True)
        outputs.extend(render_phase_noise(args.phase_noise_csv, output_dir))

    for path in outputs:
        print(f"Wrote {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
