#!/usr/bin/env python3
"""Download Databento MBP-1 top-of-book data for C++ backtest replay.

Output format:
  step,bid_price,bid_size,ask_price,ask_size

This is the L1/backtest input used by the C++ broker for buy-side ranking.
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
    parser.add_argument("--dataset", default="EQUS.MINI")
    parser.add_argument("--schema", default="mbp-1")
    parser.add_argument("--start", default="")
    parser.add_argument("--end", default="")
    parser.add_argument("--output", required=True)
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


def write_synthetic(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["step", "bid_price", "bid_size", "ask_price", "ask_size"])
        for step in range(200):
            mid = 100.0 + 0.01 * step
            writer.writerow(
                [step, f"{mid - 0.01:.4f}", 1000.0, f"{mid + 0.01:.4f}", 1000.0]
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
        writer.writerow(["step", "bid_price", "bid_size", "ask_price", "ask_size"])
        for step, (_, row) in enumerate(df.iterrows()):
            bid_px = row.get("bid_px_00")
            ask_px = row.get("ask_px_00")
            bid_sz = row.get("bid_sz_00", 0)
            ask_sz = row.get("ask_sz_00", 0)
            if (
                finite_positive(bid_px)
                and finite_positive(ask_px)
                and finite_positive(bid_sz)
                and finite_positive(ask_sz)
            ):
                writer.writerow(
                    [step, float(bid_px), float(bid_sz), float(ask_px), float(ask_sz)]
                )


def main() -> int:
    args = parse_args()
    path = Path(args.output)
    if args.synthetic or args.dataset == "synthetic":
        write_synthetic(path)
        return 0

    write_databento(args, path)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"databento_download_mbp1.py: {exc}", file=sys.stderr)
        raise
