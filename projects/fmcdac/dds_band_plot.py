#!/usr/bin/env python3
"""
Helpers for DDS-band summary extraction and SVG/CSV plotting.
"""

from __future__ import annotations

import json
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List, Optional, Sequence, Tuple


@dataclass(frozen=True)
class DdsBandPoint:
    frequency_mhz: float
    level_value: float
    level_unit: str
    delta_db: Optional[float]
    name: str


@dataclass(frozen=True)
class DdsBandSeries:
    label: str
    summary_path: Path
    points: List[DdsBandPoint]


def _escape_xml(text: str) -> str:
    return (
        text.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
    )


def _extract_level(metrics: dict) -> Tuple[float, str, Optional[float]]:
    if metrics.get("power_dbm") is not None:
        return (
            float(metrics["power_dbm"]),
            "dBm",
            None if metrics.get("power_delta_db") is None else float(metrics["power_delta_db"]),
        )

    return (
        float(metrics.get("vpp_v", 0.0)),
        "Vpp",
        None if metrics.get("vpp_delta_db") is None else float(metrics["vpp_delta_db"]),
    )


def load_dds_band_series(summary_path: Path, label: Optional[str] = None) -> DdsBandSeries:
    summary_path = Path(summary_path)
    data = json.loads(summary_path.read_text(encoding="utf-8"))
    points: List[DdsBandPoint] = []

    for step in data.get("steps", []):
        if step.get("group") != "dds_band":
            continue
        expected = step.get("expected_freq_hz") or []
        metrics = step.get("metrics") or {}
        if not expected:
            continue
        level_value, level_unit, delta_db = _extract_level(metrics)
        points.append(
            DdsBandPoint(
                frequency_mhz=float(expected[0]) / 1e6,
                level_value=level_value,
                level_unit=level_unit,
                delta_db=delta_db,
                name=str(step.get("name", "")),
            )
        )

    points.sort(key=lambda item: item.frequency_mhz)
    if label is None:
        label = str(data.get("interp_mode") or summary_path.parent.name or summary_path.stem)
    return DdsBandSeries(label=label, summary_path=summary_path, points=points)


def load_many(summary_paths: Iterable[Path], labels: Optional[Sequence[str]] = None) -> List[DdsBandSeries]:
    paths = [Path(item) for item in summary_paths]
    if labels is not None and len(labels) != len(paths):
        raise ValueError("Number of labels must match number of summary paths")
    return [
        load_dds_band_series(path, None if labels is None else labels[index])
        for index, path in enumerate(paths)
    ]


def write_dds_band_csv(series_list: Sequence[DdsBandSeries], output_path: Path) -> None:
    output_path = Path(output_path)
    freqs = sorted({point.frequency_mhz for series in series_list for point in series.points})

    header = ["frequency_mhz"]
    for series in series_list:
        header.append(f"{series.label}_level")
        header.append(f"{series.label}_level_unit")
        header.append(f"{series.label}_delta_db")
    rows = [",".join(header)]

    for freq in freqs:
        row = [f"{freq:.6f}"]
        for series in series_list:
            point = next((item for item in series.points if item.frequency_mhz == freq), None)
            if point is None:
                row.extend(["", "", ""])
            else:
                row.append(f"{point.level_value:.9f}")
                row.append(point.level_unit)
                row.append("" if point.delta_db is None else f"{point.delta_db:.6f}")
        rows.append(",".join(row))

    output_path.write_text("\n".join(rows) + "\n", encoding="utf-8")


def _nice_floor(value: float, step: float) -> float:
    return math.floor(value / step) * step


def _nice_ceil(value: float, step: float) -> float:
    return math.ceil(value / step) * step


def write_dds_band_svg(
    series_list: Sequence[DdsBandSeries],
    output_path: Path,
    title: str = "DDS Band Level Delta vs Frequency",
) -> None:
    output_path = Path(output_path)
    if not series_list:
        raise ValueError("No DDS-band series supplied")

    all_points = [point for series in series_list for point in series.points]
    if not all_points:
        raise ValueError("No DDS-band points found in supplied summaries")

    x_values = [point.frequency_mhz for point in all_points]
    y_values = [point.delta_db for point in all_points if point.delta_db is not None]
    if not y_values:
        y_values = [point.level_value for point in all_points]

    x_min = min(x_values)
    x_max = max(x_values)
    y_min = min(y_values)
    y_max = max(y_values)
    y_top = max(0.0, _nice_ceil(y_max, 2.0))
    y_bottom = _nice_floor(min(y_min, -1.0), 5.0)
    if y_top <= y_bottom:
        y_top = y_bottom + 5.0

    width = 1100
    height = 680
    left = 90
    right = 40
    top = 70
    bottom = 90
    plot_w = width - left - right
    plot_h = height - top - bottom
    colors = ["#0b7285", "#c92a2a", "#2b8a3e", "#e67700", "#5f3dc4"]

    def sx(value: float) -> float:
        return left + ((value - x_min) / (x_max - x_min or 1.0)) * plot_w

    def sy(value: float) -> float:
        return top + ((y_top - value) / (y_top - y_bottom or 1.0)) * plot_h

    svg: List[str] = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#f8f9fa"/>',
        f'<text x="{width/2:.1f}" y="34" text-anchor="middle" font-size="24" font-family="Segoe UI, Arial, sans-serif" fill="#212529">{_escape_xml(title)}</text>',
        f'<text x="{width/2:.1f}" y="58" text-anchor="middle" font-size="13" font-family="Segoe UI, Arial, sans-serif" fill="#495057">Relative amplitude in dB, normalized to the first DDS-band reference point in each run</text>',
        f'<rect x="{left}" y="{top}" width="{plot_w}" height="{plot_h}" fill="#ffffff" stroke="#adb5bd" stroke-width="1"/>',
    ]

    y_tick = y_bottom
    while y_tick <= y_top + 1e-9:
        y = sy(y_tick)
        svg.append(f'<line x1="{left}" y1="{y:.2f}" x2="{left + plot_w}" y2="{y:.2f}" stroke="#e9ecef" stroke-width="1"/>')
        svg.append(f'<text x="{left - 12}" y="{y + 4:.2f}" text-anchor="end" font-size="12" font-family="Segoe UI, Arial, sans-serif" fill="#495057">{y_tick:.0f} dB</text>')
        y_tick += 5.0

    x_tick = x_min
    while x_tick <= x_max + 1e-9:
        x = sx(x_tick)
        svg.append(f'<line x1="{x:.2f}" y1="{top}" x2="{x:.2f}" y2="{top + plot_h}" stroke="#f1f3f5" stroke-width="1"/>')
        svg.append(f'<text x="{x:.2f}" y="{top + plot_h + 24}" text-anchor="middle" font-size="12" font-family="Segoe UI, Arial, sans-serif" fill="#495057">{x_tick:.0f}</text>')
        x_tick += 10.0

    svg.append(f'<text x="{left + plot_w / 2:.2f}" y="{height - 30}" text-anchor="middle" font-size="14" font-family="Segoe UI, Arial, sans-serif" fill="#212529">Frequency (MHz)</text>')
    svg.append(f'<text x="24" y="{top + plot_h / 2:.2f}" text-anchor="middle" font-size="14" font-family="Segoe UI, Arial, sans-serif" fill="#212529" transform="rotate(-90 24 {top + plot_h / 2:.2f})">Amplitude Delta (dB)</text>')

    legend_x = left + 10
    legend_y = 26
    for index, series in enumerate(series_list):
        color = colors[index % len(colors)]
        y = legend_y + index * 20
        svg.append(f'<line x1="{legend_x}" y1="{y}" x2="{legend_x + 28}" y2="{y}" stroke="{color}" stroke-width="3"/>')
        svg.append(f'<circle cx="{legend_x + 14}" cy="{y}" r="4" fill="{color}"/>')
        svg.append(f'<text x="{legend_x + 38}" y="{y + 4}" font-size="13" font-family="Segoe UI, Arial, sans-serif" fill="#212529">{_escape_xml(series.label)}</text>')

    for index, series in enumerate(series_list):
        color = colors[index % len(colors)]
        coords = []
        for point in series.points:
            y_value = point.delta_db if point.delta_db is not None else point.level_value
            coords.append((sx(point.frequency_mhz), sy(y_value), point))
        polyline = " ".join(f"{x:.2f},{y:.2f}" for x, y, _ in coords)
        svg.append(f'<polyline fill="none" stroke="{color}" stroke-width="3" points="{polyline}"/>')
        for x, y, point in coords:
            tooltip = f"{series.label}: {point.frequency_mhz:.0f} MHz, Level={point.level_value:.3f} {point.level_unit}"
            if point.delta_db is not None:
                tooltip += f", Delta={point.delta_db:.2f} dB"
            svg.append(
                f'<g><title>{_escape_xml(tooltip)}</title>'
                f'<circle cx="{x:.2f}" cy="{y:.2f}" r="4.5" fill="{color}" stroke="#ffffff" stroke-width="1.5"/></g>'
            )

    svg.append("</svg>")
    output_path.write_text("\n".join(svg) + "\n", encoding="utf-8")
