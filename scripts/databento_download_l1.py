#!/usr/bin/env python3
"""Download Databento MBP-1 (top-of-book) data into the C++ backtest CSV
replay format.

Output schema (matches what the broker's `parse_top_row` expects):
    ts_event,step,bid_price,bid_size,ask_price,ask_size

One row per top-of-book update. `step` is a 0-based monotonic counter
across the file. `ts_event` is nanoseconds since the Unix epoch (raw
exchange timestamp; NOT split-adjusted - that's the whole point of
moving off the IBKR L1 source).

This script is the L1 analog of databento_download_l2.py. CLI mirrors
it so the same broker invocation pattern works for both (the broker's
l1_downloader_command builder just swaps schema mbp-10 -> mbp-1 and the
output filename suffix). You can also run it standalone:

    .venv/bin/python scripts/databento_download_l1.py \\
        --symbol AAPL \\
        --start 2024-08-02T13:30:00Z \\
        --end   2024-08-09T20:00:00Z \\
        --output data/l1/2024-08-02_2024-08-09/AAPL_20240802T133000Z_20240809T200000Z.mbp1.csv

Costs ~$0.15-0.30 per symbol per 5-10 day window (vs $0.50-2 for the
L2 schema). See scripts/databento_l1_cost_quote.py for exact quotes.
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
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--symbol", required=True)
    p.add_argument("--dataset", default="XNAS.ITCH",
                   help="Databento dataset (XNAS.ITCH for NASDAQ symbols).")
    p.add_argument("--schema", default="mbp-1")
    p.add_argument("--stype-in", default="raw_symbol")
    p.add_argument("--stype-out", default="instrument_id")
    p.add_argument("--start", default="")
    p.add_argument("--end", default="")
    p.add_argument("--output", required=True)
    p.add_argument("--max-records", type=int, default=0)
    p.add_argument("--chunk-days", type=int, default=0)
    p.add_argument(
        "--chunk-hours", type=int, default=1,
        help="Chunk by ticker/hour by default (matches L2 downloader).",
    )
    p.add_argument(
        "--api-key-file",
        default=os.environ.get("DATABENTO_API_KEY_FILE", ""),
        help="File containing the Databento API key. Defaults to "
             "~/.config/trading-system/databento_api_key when present.",
    )
    p.add_argument(
        "--synthetic", action="store_true",
        help="Write deterministic sample data without contacting Databento.",
    )
    return p.parse_args()


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


def write_synthetic(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    base_ns = int(
        dt.datetime(2026, 1, 1, 13, 30, tzinfo=dt.timezone.utc).timestamp()
        * 1_000_000_000
    )
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["ts_event", "step", "bid_price", "bid_size", "ask_price", "ask_size"])
        for step in range(200):
            ts_event = base_ns + step * 1_000_000_000
            mid = 100.0 + 0.01 * step
            w.writerow([ts_event, step, f"{mid - 0.005:.4f}", 100, f"{mid + 0.005:.4f}", 100])


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
        w = csv.writer(f)
        w.writerow(["ts_event", "step", "bid_price", "bid_size", "ask_price", "ask_size"])
        for start, end in iter_windows(
            args.start, args.end, args.chunk_hours, args.chunk_days,
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
            for idx, row in df.iterrows():
                # ts_event is a column in MBP-1 frames; fall back to the index.
                ts_event_val = row.get("ts_event", None)
                if ts_event_val is None:
                    ts_event_val = idx
                try:
                    ts_event_ns = int(ts_event_val.value)
                except AttributeError:
                    ts_event_ns = int(ts_event_val)

                # MBP-1 surfaces top-of-book via bid_px_00 / ask_px_00.
                bid_px = row.get("bid_px_00")
                ask_px = row.get("ask_px_00")
                bid_sz = row.get("bid_sz_00", 0)
                ask_sz = row.get("ask_sz_00", 0)
                if not (finite_positive(bid_px) and finite_positive(ask_px)):
                    continue
                w.writerow([
                    ts_event_ns, step,
                    float(bid_px), float(bid_sz),
                    float(ask_px), float(ask_sz),
                ])
                step += 1
            if remaining > 0:
                remaining -= len(df)
                if remaining <= 0:
                    break


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
        print(f"databento_download_l1.py: {exc}", file=sys.stderr)
        raise
