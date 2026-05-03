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
import datetime as dt
import math
import os
import sys
from pathlib import Path

DEFAULT_API_KEY_FILE = Path.home() / ".config" / "trading-system" / "databento_api_key"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--symbol", required=True)
    parser.add_argument("--dataset", default="XNAS.ITCH")
    parser.add_argument("--schema", default="mbp-10")
    parser.add_argument("--stype-in", default="raw_symbol")
    parser.add_argument("--stype-out", default="instrument_id")
    parser.add_argument("--start", default="")
    parser.add_argument("--end", default="")
    parser.add_argument("--output", required=True)
    parser.add_argument("--depth", type=int, default=5)
    parser.add_argument("--max-records", type=int, default=0)
    parser.add_argument("--chunk-days", type=int, default=0)
    parser.add_argument(
        "--chunk-hours",
        type=int,
        default=1,
        help="Chunk L2 downloads by ticker/hour by default.",
    )
    parser.add_argument(
        "--api-key-file",
        default=os.environ.get("DATABENTO_API_KEY_FILE", ""),
        help="Optional file containing the Databento API key. Defaults to ~/.config/trading-system/databento_api_key when present.",
    )
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


def read_api_key_file(path_text: str) -> str:
    path = Path(path_text).expanduser()
    if not path.exists():
        raise FileNotFoundError(f"Databento API key file not found: {path}")
    for line in path.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if stripped.startswith("DATABENTO_API_KEY="):
            return stripped.split("=", 1)[1].strip().strip("'\"")
        return stripped.strip("'\"")
    raise ValueError(f"Databento API key file is empty: {path}")


def parse_time(value: str) -> dt.datetime:
    parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=dt.timezone.utc)
    return parsed.astimezone(dt.timezone.utc)


def format_time(value: dt.datetime) -> str:
    return value.astimezone(dt.timezone.utc).isoformat().replace("+00:00", "Z")


def chunk_delta(chunk_hours: int, chunk_days: int) -> dt.timedelta | None:
    if chunk_hours > 0:
        return dt.timedelta(hours=chunk_hours)
    if chunk_days > 0:
        return dt.timedelta(days=chunk_days)
    return None


def iter_windows(start: str, end: str, chunk_hours: int, chunk_days: int):
    delta = chunk_delta(chunk_hours, chunk_days)
    if not end or delta is None:
        yield start, end
        return
    cursor = parse_time(start)
    final = parse_time(end)
    while cursor < final:
        next_cursor = min(cursor + delta, final)
        yield format_time(cursor), format_time(next_cursor)
        cursor = next_cursor


def configure_databento_key(path_text: str) -> None:
    path = Path(path_text).expanduser() if path_text else DEFAULT_API_KEY_FILE
    if path.exists():
        os.environ["DATABENTO_API_KEY"] = read_api_key_file(str(path))


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

    configure_databento_key(args.api_key_file)
    client = db.Historical()

    path.parent.mkdir(parents=True, exist_ok=True)
    remaining = args.max_records
    step = 0
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["step", "side", "level", "price", "size"])
        for start, end in iter_windows(
            args.start,
            args.end,
            args.chunk_hours,
            args.chunk_days,
        ):
            request = {
                "dataset": args.dataset,
                "schema": args.schema,
                "symbols": [args.symbol],
                "start": start,
                "stype_in": args.stype_in,
                "stype_out": args.stype_out,
            }
            if end:
                request["end"] = end
            if remaining > 0:
                request["limit"] = remaining

            data = client.timeseries.get_range(**request)
            df = data.to_df(price_type="float", map_symbols=True)
            for _, row in df.iterrows():
                wrote_level = False
                for level in range(args.depth):
                    suffix = f"{level:02d}"
                    bid_px = row.get(f"bid_px_{suffix}")
                    ask_px = row.get(f"ask_px_{suffix}")
                    bid_sz = row.get(f"bid_sz_{suffix}", 0)
                    ask_sz = row.get(f"ask_sz_{suffix}", 0)
                    if finite_positive(bid_px) and finite_positive(bid_sz):
                        writer.writerow(
                            [step, "bid", level, float(bid_px), float(bid_sz)]
                        )
                        wrote_level = True
                    if finite_positive(ask_px) and finite_positive(ask_sz):
                        writer.writerow(
                            [step, "ask", level, float(ask_px), float(ask_sz)]
                        )
                        wrote_level = True
                if wrote_level:
                    step += 1
            if remaining > 0:
                remaining -= len(df)
                if remaining <= 0:
                    break


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
