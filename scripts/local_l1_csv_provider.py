#!/usr/bin/env python3
"""Provide L1 backtest CSV from a local/external source.

Output format:
  step,bid_price,bid_size,ask_price,ask_size

The source can be either:
  - an already-normalized MBP-1 CSV with bid/ask columns
  - an OHLC/minute-bar CSV with a close price, used as a coarse L1 proxy

This script intentionally does not contact Databento.
"""

from __future__ import annotations

import argparse
import csv
import math
import os
import shutil
import sys
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--symbol", required=True)
    parser.add_argument(
        "--dataset",
        default=os.environ.get("L1_SOURCE_DIR", "data/l1"),
        help="Local directory containing external L1 CSV files.",
    )
    parser.add_argument("--schema", default="mbp-1")
    parser.add_argument("--start", default="")
    parser.add_argument("--end", default="")
    parser.add_argument("--output", required=True)
    parser.add_argument("--max-records", type=int, default=0)
    parser.add_argument("--chunk-hours", type=int, default=0)
    parser.add_argument("--chunk-days", type=int, default=0)
    parser.add_argument("--synthetic", action="store_true")
    return parser.parse_args()


def safe_symbol(symbol: str) -> str:
    return "".join(ch if ch.isalnum() else "_" for ch in symbol) or "symbol"


def finite_positive(value: object) -> bool:
    try:
        v = float(value)
    except (TypeError, ValueError):
        return False
    return math.isfinite(v) and v > 0.0


def candidate_paths(source_dir: Path, symbol: str) -> list[Path]:
    safe = safe_symbol(symbol)
    return [
        source_dir / f"{safe}.mbp1.csv",
        source_dir / f"{symbol}.mbp1.csv",
        source_dir / f"{safe}.csv",
        source_dir / f"{symbol}.csv",
    ]


def find_source(source_dir: Path, symbol: str) -> Path:
    for path in candidate_paths(source_dir, symbol):
        if path.exists():
            return path
    names = ", ".join(str(path) for path in candidate_paths(source_dir, symbol))
    raise FileNotFoundError(f"No external L1 CSV found. Tried: {names}")


def row_value(row: dict[str, str], *names: str) -> str:
    for name in names:
        if name in row and row[name] != "":
            return row[name]
    return ""


def write_synthetic(path: Path, max_records: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    rows = max_records if max_records > 0 else 200
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["step", "bid_price", "bid_size", "ask_price", "ask_size"])
        for step in range(rows):
            mid = 100.0 + 0.01 * step
            writer.writerow(
                [step, f"{mid - 0.01:.4f}", 1000.0, f"{mid + 0.01:.4f}", 1000.0]
            )


def write_normalized(source: Path, output: Path, max_records: int) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    with source.open(newline="", encoding="utf-8") as src, output.open(
        "w", newline="", encoding="utf-8"
    ) as dst:
        reader = csv.DictReader(src)
        writer = csv.writer(dst)
        writer.writerow(["step", "bid_price", "bid_size", "ask_price", "ask_size"])

        for step, row in enumerate(reader):
            if max_records > 0 and step >= max_records:
                break

            bid = row_value(row, "bid_price", "bid", "best_bid", "bid_px_00")
            ask = row_value(row, "ask_price", "ask", "best_ask", "ask_px_00")
            bid_size = row_value(row, "bid_size", "bid_sz", "bid_sz_00")
            ask_size = row_value(row, "ask_size", "ask_sz", "ask_sz_00")

            if finite_positive(bid) and finite_positive(ask):
                writer.writerow(
                    [
                        step,
                        float(bid),
                        float(bid_size) if finite_positive(bid_size) else 1.0,
                        float(ask),
                        float(ask_size) if finite_positive(ask_size) else 1.0,
                    ]
                )
                continue

            close = row_value(row, "close", "Close", "price", "mid", "last")
            if finite_positive(close):
                mid = float(close)
                writer.writerow([step, mid, 1.0, mid, 1.0])


def main() -> int:
    args = parse_args()
    output = Path(args.output)
    if args.synthetic:
        write_synthetic(output, args.max_records)
        return 0

    source_dir = Path(args.dataset).expanduser()
    source = find_source(source_dir, args.symbol)
    if source.resolve() == output.resolve():
        return 0

    if source.name.endswith(".mbp1.csv") and args.max_records <= 0:
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, output)
        return 0

    write_normalized(source, output, args.max_records)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"local_l1_csv_provider.py: {exc}", file=sys.stderr)
        raise
