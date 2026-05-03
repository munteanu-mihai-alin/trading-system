#!/usr/bin/env python3
"""Run a Databento-backed sell-window backtest with hftbacktest.

Workflow:
  1. Download/reuse Databento MBP-1 for the buy/ranking/top-of-book side.
  2. Pick an entry reference from MBP-1.
  3. Download/reuse Databento MBP-10 for the sell/execution window.
  4. Convert MBP-10 replay CSV into hftbacktest's normalized depth format.
  5. Start hftbacktest with an open long position and submit the target SELL.

This mirrors the C++ split: L1 for buy-side queries, L2 for sell-side execution.
"""

from __future__ import annotations

import argparse
import csv
import json
import subprocess
import sys
from pathlib import Path
from typing import Iterable

import numpy as np


DEPTH_EVENT = 1
BID_SIDE = 1
ASK_SIDE = -1


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--symbol", required=True)
    parser.add_argument("--start", required=True)
    parser.add_argument("--end", default="")
    parser.add_argument("--cache-dir", default="data/databento")
    parser.add_argument("--l1-dataset", default="EQUS.MINI")
    parser.add_argument("--l2-dataset", default="XNAS.ITCH")
    parser.add_argument("--l1-script", default="scripts/databento_download_mbp1.py")
    parser.add_argument("--l2-script", default="scripts/databento_download_l2.py")
    parser.add_argument("--python", default=sys.executable)
    parser.add_argument("--qty", type=float, default=1.0)
    parser.add_argument("--target-profit-pct", type=float, default=0.008)
    parser.add_argument("--cost-per-share", type=float, default=0.0)
    parser.add_argument("--tick-size", type=float, default=0.01)
    parser.add_argument("--lot-size", type=float, default=1.0)
    parser.add_argument("--maker-fee", type=float, default=0.0)
    parser.add_argument("--taker-fee", type=float, default=0.0)
    parser.add_argument("--feed-latency-ns", type=int, default=1_000_000)
    parser.add_argument("--order-latency-ns", type=int, default=1_000_000)
    parser.add_argument("--step-ns", type=int, default=10_000_000)
    parser.add_argument("--synthetic", action="store_true")
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--summary", default="")
    return parser.parse_args()


def safe_symbol(symbol: str) -> str:
    return "".join(ch if ch.isalnum() else "_" for ch in symbol) or "symbol"


def run(cmd: list[str]) -> None:
    subprocess.run(cmd, check=True)


def ensure_data(args: argparse.Namespace, path: Path, script: str, dataset: str, schema: str) -> None:
    if path.exists():
        return
    cmd = [
        args.python,
        script,
        "--symbol",
        args.symbol,
        "--dataset",
        "synthetic" if args.synthetic else dataset,
        "--schema",
        schema,
        "--start",
        args.start,
        "--output",
        str(path),
    ]
    if args.end:
        cmd.extend(["--end", args.end])
    if args.synthetic:
        cmd.append("--synthetic")
    run(cmd)


def read_l1_entry(path: Path) -> tuple[int, float]:
    with path.open(newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            ask = float(row["ask_price"])
            if ask > 0:
                return int(row["step"]), ask
    raise SystemExit(f"no valid ask price in {path}")


def read_l2_rows(path: Path) -> Iterable[tuple[int, str, int, float, float]]:
    with path.open(newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            yield (
                int(row["step"]),
                row["side"],
                int(row["level"]),
                float(row["price"]),
                float(row["size"]),
            )


def convert_l2_to_hftbacktest(path: Path, feed_latency_ns: int) -> np.ndarray:
    rows: list[tuple[float, float, float, float, float, float]] = []
    for step, side, _level, price, size in read_l2_rows(path):
        if price <= 0 or size <= 0:
            continue
        exch_ts = float(step * feed_latency_ns)
        local_ts = float(exch_ts + feed_latency_ns)
        hft_side = BID_SIDE if side.lower() in {"bid", "b", "0"} else ASK_SIDE
        rows.append((DEPTH_EVENT, exch_ts, local_ts, hft_side, price, size))
    if not rows:
        raise SystemExit(f"no valid L2 rows in {path}")
    return np.asarray(rows, dtype=np.float64)


def import_hftbacktest():
    try:
        import hftbacktest as hft  # type: ignore
    except ImportError as exc:
        raise SystemExit("Install hftbacktest first: pip install hftbacktest") from exc

    def attr(name: str, *fallback_modules: str):
        if hasattr(hft, name):
            return getattr(hft, name)
        for module_name in fallback_modules:
            try:
                module = __import__(module_name, fromlist=[name])
                return getattr(module, name)
            except (ImportError, AttributeError):
                continue
        raise AttributeError(name)

    asset_type = hft.Linear if hasattr(hft, "Linear") else attr(
        "LinearAsset",
        "hftbacktest.assettype",
        "hftbacktest.asset_type",
    )()

    return {
        "HftBacktest": attr("HftBacktest"),
        "ConstantLatency": attr(
            "ConstantLatency",
            "hftbacktest.models.latencies",
            "hftbacktest.models.latency",
        ),
        "RiskAverseQueueModel": attr(
            "RiskAverseQueueModel",
            "hftbacktest.models.queue",
            "hftbacktest.models.queues",
        ),
        "asset_type": asset_type,
        "GTC": getattr(hft, "GTC", 0),
    }


def run_sell_window_backtest(args: argparse.Namespace, data: np.ndarray, entry_price: float) -> dict:
    hft = import_hftbacktest()
    sell_limit = entry_price * (1.0 + args.target_profit_pct) + args.cost_per_share
    hbt = hft["HftBacktest"](
        data,
        tick_size=args.tick_size,
        lot_size=args.lot_size,
        maker_fee=args.maker_fee,
        taker_fee=args.taker_fee,
        order_latency=hft["ConstantLatency"](
            args.order_latency_ns,
            args.order_latency_ns,
        ),
        asset_type=hft["asset_type"],
        queue_model=hft["RiskAverseQueueModel"](),
        start_position=args.qty,
        start_balance=-(entry_price * args.qty),
    )

    order_id = 1
    submitted = False
    filled = False
    while hbt.run:
        if not hbt.elapse(args.step_ns):
            break
        hbt.clear_inactive_orders()
        if not submitted:
            hbt.submit_sell_order(order_id, sell_limit, args.qty, hft["GTC"], wait=False)
            hbt.wait_order_response(order_id)
            submitted = True
        if hbt.position <= 0:
            filled = True
            break

    return {
        "symbol": args.symbol,
        "entry_price": entry_price,
        "sell_limit": sell_limit,
        "qty": args.qty,
        "filled": filled,
        "final_position": float(hbt.position),
        "balance": float(hbt.balance),
        "equity": float(hbt.equity),
    }


def main() -> int:
    args = parse_args()
    cache = Path(args.cache_dir)
    symbol_file = safe_symbol(args.symbol)
    l1_csv = cache / f"{symbol_file}.mbp1.csv"
    l2_csv = cache / f"{symbol_file}.mbp10.csv"
    hft_npy = cache / f"{symbol_file}.hftbacktest.npy"

    ensure_data(args, l1_csv, args.l1_script, args.l1_dataset, "mbp-1")
    entry_step, entry_price = read_l1_entry(l1_csv)
    ensure_data(args, l2_csv, args.l2_script, args.l2_dataset, "mbp-10")
    data = convert_l2_to_hftbacktest(l2_csv, args.feed_latency_ns)
    hft_npy.parent.mkdir(parents=True, exist_ok=True)
    np.save(hft_npy, data)

    summary = {
        "symbol": args.symbol,
        "entry_step": entry_step,
        "entry_price": entry_price,
        "l1_csv": str(l1_csv),
        "l2_csv": str(l2_csv),
        "hftbacktest_data": str(hft_npy),
    }
    if not args.prepare_only:
        summary.update(run_sell_window_backtest(args, data, entry_price))

    text = json.dumps(summary, indent=2, sort_keys=True)
    if args.summary:
        Path(args.summary).write_text(text + "\n", encoding="utf-8")
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
