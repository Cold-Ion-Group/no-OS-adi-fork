#!/usr/bin/env python3
"""
Generate DDS-band CSV/SVG plots from one or more FMCDAC summary.json files.
"""

from __future__ import annotations

import argparse
from pathlib import Path

from dds_band_plot import load_many, write_dds_band_csv, write_dds_band_svg


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Plot DDS-band amplitude from one or more FMCDAC summary.json files."
    )
    parser.add_argument("summary", nargs="+", help="Path(s) to summary.json")
    parser.add_argument("--labels", nargs="*", default=None, help="Optional labels matching the summary paths")
    parser.add_argument("--output-svg", default=None, help="Output SVG path")
    parser.add_argument("--output-csv", default=None, help="Output CSV path")
    parser.add_argument("--title", default="DDS Band Level Delta vs Frequency", help="Plot title")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    summaries = [Path(item).resolve() for item in args.summary]
    series = load_many(summaries, args.labels)

    base_dir = summaries[0].parent
    svg_path = Path(args.output_svg) if args.output_svg else base_dir / "dds_band_plot.svg"
    csv_path = Path(args.output_csv) if args.output_csv else base_dir / "dds_band_plot.csv"

    write_dds_band_csv(series, csv_path)
    write_dds_band_svg(series, svg_path, args.title)
    print(f"Wrote {svg_path}")
    print(f"Wrote {csv_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
