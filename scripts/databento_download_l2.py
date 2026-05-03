#!/usr/bin/env python3
"""Download Databento MBP-10 data into the C++ backtest CSV replay format.

Output format:
  step,side,level,price,size

Each step represents one Databento MBP-10 row. For each row, this script emits
the top N bid and ask levels that the C++ L2Book can replay.
"""

from __future__ import annotations

import argparse
import csv
import math
import sys
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--symbol", required=True)
    parser.add_argument("--dataset", default="XNAS.ITCH")
    parser.add_argument("--schema", default="mbp-10")
    parser.add_argument("--start", default="")
    parser.add_argument("--end", default="")
    parser.add_argument("--output", required=True)
    parser.add_argument("--depth", type=int, default=5)
    parser.add_argument("--max-records", type=int, default=0)
    parser.add_argument(
        "--synthetic",
        action="store_true",
        help="Write deterministic sample data without contacting Databento.",
    )
    return parser.parse_args()


def finite_positive(value: object) -> bool:
    try:
        v = float(value)
    except (TypeError, ValueError):
        return False
    return math.isfinite(v) and v > 0.0


def write_synthetic(path: Path, depth: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["step", "side", "level", "price", "size"])
        for step in range(200):
            mid = 100.0 + 0.01 * step
            for level in range(depth):
                writer.writerow(
                    [step, "bid", level, f"{mid - 0.01 * (level + 1):.4f}", 1000 - 25 * level]
                )
                writer.writerow(
                    [step, "ask", level, f"{mid + 0.01 * (level + 1):.4f}", 1000 - 25 * level]
                )


def write_databento(args: argparse.Namespace, path: Path) -> None:
    if not args.start:
        raise SystemExit("--start is required for Databento downloads")

    try:
        import databento as db  # type: ignore
    except ImportError as exc:
        raise SystemExit(
            "Python package 'databento' is not installed. Run: pip install -U databento"
        ) from exc

    client = db.Historical()
    request = {
        "dataset": args.dataset,
        "schema": args.schema,
        "symbols": [args.symbol],
        "start": args.start,
        "stype_in": "raw_symbol",
        "stype_out": "raw_symbol",
    }
    if args.end:
        request["end"] = args.end
    if args.max_records > 0:
        request["limit"] = args.max_records

    data = client.timeseries.get_range(**request)
    df = data.to_df(price_type="float", map_symbols=True)

    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["step", "side", "level", "price", "size"])
        for step, (_, row) in enumerate(df.iterrows()):
            for level in range(args.depth):
                suffix = f"{level:02d}"
                bid_px = row.get(f"bid_px_{suffix}")
                ask_px = row.get(f"ask_px_{suffix}")
                bid_sz = row.get(f"bid_sz_{suffix}", 0)
                ask_sz = row.get(f"ask_sz_{suffix}", 0)
                if finite_positive(bid_px) and finite_positive(bid_sz):
                    writer.writerow([step, "bid", level, float(bid_px), float(bid_sz)])
                if finite_positive(ask_px) and finite_positive(ask_sz):
                    writer.writerow([step, "ask", level, float(ask_px), float(ask_sz)])


def main() -> int:
    args = parse_args()
    path = Path(args.output)
    if args.synthetic or args.dataset == "synthetic":
        write_synthetic(path, args.depth)
        return 0

    write_databento(args, path)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"databento_download_l2.py: {exc}", file=sys.stderr)
        raise
