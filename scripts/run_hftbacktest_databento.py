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
from typing import Iterable, Sequence

import numpy as np


DEFAULT_BACKTEST_START = "2026-04-13T13:30:00Z"
DEFAULT_BACKTEST_END = "2026-05-01T20:00:00Z"
DEFAULT_BACKTEST_SYMBOLS = ["AAPL", "NVDA", "AMD"]
DEFAULT_REPORT_PATH = "reports/databento_backtest_report.md"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--symbol", default="")
    parser.add_argument(
        "--symbols",
        default=",".join(DEFAULT_BACKTEST_SYMBOLS),
        help="Comma-separated symbols. Ignored when --symbol is set.",
    )
    parser.add_argument("--start", default=DEFAULT_BACKTEST_START)
    parser.add_argument("--end", default=DEFAULT_BACKTEST_END)
    parser.add_argument("--cache-dir", default="data/databento")
    parser.add_argument("--l1-dataset", default="EQUS.MINI")
    parser.add_argument("--l2-dataset", default="XNAS.ITCH")
    parser.add_argument("--l1-script", default="scripts/databento_download_mbp1.py")
    parser.add_argument("--l2-script", default="scripts/databento_download_l2.py")
    parser.add_argument("--python", default=sys.executable)
    parser.add_argument(
        "--api-key-file",
        default="",
        help="Optional private Databento API key file path passed to downloader scripts.",
    )
    parser.add_argument("--max-records", type=int, default=0)
    parser.add_argument("--l1-chunk-hours", type=int, default=24)
    parser.add_argument("--l2-chunk-hours", type=int, default=1)
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
    parser.add_argument("--report", default=DEFAULT_REPORT_PATH)
    return parser.parse_args()


def safe_symbol(symbol: str) -> str:
    return "".join(ch if ch.isalnum() else "_" for ch in symbol) or "symbol"


def run(cmd: list[str]) -> None:
    subprocess.run(cmd, check=True)


def parse_symbols(args: argparse.Namespace) -> list[str]:
    raw = args.symbol if args.symbol else args.symbols
    symbols = [item.strip().upper() for item in raw.split(",") if item.strip()]
    if not symbols:
        raise SystemExit("at least one symbol is required")
    return symbols


def ensure_data(
    args: argparse.Namespace,
    symbol: str,
    path: Path,
    script: str,
    dataset: str,
    schema: str,
    chunk_hours: int,
) -> None:
    if path.exists():
        return
    cmd = [
        args.python,
        script,
        "--symbol",
        symbol,
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
    if args.max_records > 0:
        cmd.extend(["--max-records", str(args.max_records)])
    if chunk_hours > 0:
        cmd.extend(["--chunk-hours", str(chunk_hours)])
    if args.api_key_file:
        cmd.extend(["--api-key-file", args.api_key_file])
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


def collect_l2_metrics(
    path: Path,
    entry_price: float,
    sell_limit: float,
) -> dict:
    by_step: dict[int, dict[str, float]] = {}
    row_count = 0
    bid_rows = 0
    ask_rows = 0
    bid_depth_total = 0.0
    ask_depth_total = 0.0

    for step, side, level, price, size in read_l2_rows(path):
        row_count += 1
        side_l = side.lower()
        if side_l in {"bid", "b", "0"}:
            bid_rows += 1
            bid_depth_total += size
            if level == 0:
                by_step.setdefault(step, {})["bid_price"] = price
                by_step.setdefault(step, {})["bid_size"] = size
        else:
            ask_rows += 1
            ask_depth_total += size
            if level == 0:
                by_step.setdefault(step, {})["ask_price"] = price
                by_step.setdefault(step, {})["ask_size"] = size

    spreads = []
    spread_bps = []
    bid_sizes = []
    ask_sizes = []
    touch_steps = []
    for step, top in sorted(by_step.items()):
        bid = top.get("bid_price", 0.0)
        ask = top.get("ask_price", 0.0)
        if bid > 0.0 and ask > 0.0 and bid <= ask:
            spread = ask - bid
            mid = 0.5 * (bid + ask)
            spreads.append(spread)
            if mid > 0.0:
                spread_bps.append(10000.0 * spread / mid)
        bid_size = top.get("bid_size", 0.0)
        ask_size = top.get("ask_size", 0.0)
        if bid_size > 0.0:
            bid_sizes.append(bid_size)
        if ask_size > 0.0:
            ask_sizes.append(ask_size)
        if bid >= sell_limit:
            touch_steps.append(step)

    def avg(values: Sequence[float]) -> float:
        return float(sum(values) / len(values)) if values else 0.0

    return {
        "l2_rows": row_count,
        "l2_steps": len(by_step),
        "bid_rows": bid_rows,
        "ask_rows": ask_rows,
        "avg_spread": avg(spreads),
        "avg_spread_bps": avg(spread_bps),
        "avg_top_bid_size": avg(bid_sizes),
        "avg_top_ask_size": avg(ask_sizes),
        "avg_bid_depth_size_per_step": bid_depth_total / len(by_step)
        if by_step
        else 0.0,
        "avg_ask_depth_size_per_step": ask_depth_total / len(by_step)
        if by_step
        else 0.0,
        "target_sell_limit": sell_limit,
        "target_profit_per_share": sell_limit - entry_price,
        "target_touch_count": len(touch_steps),
        "target_fill_possible_from_best_bid": bool(touch_steps),
        "first_target_touch_step": touch_steps[0] if touch_steps else None,
    }


def legacy_event_rows(path: Path, feed_latency_ns: int) -> np.ndarray:
    rows: list[tuple[float, float, float, float, float, float]] = []
    for step, side, _level, price, size in read_l2_rows(path):
        if price <= 0 or size <= 0:
            continue
        exch_ts = float(step * feed_latency_ns)
        local_ts = float(exch_ts + feed_latency_ns)
        hft_side = 1.0 if side.lower() in {"bid", "b", "0"} else -1.0
        rows.append((1.0, exch_ts, local_ts, hft_side, price, size))
    if not rows:
        raise SystemExit(f"no valid L2 rows in {path}")
    return np.asarray(rows, dtype=np.float64)


def structured_event_rows(path: Path, hft, feed_latency_ns: int) -> np.ndarray:
    grouped: dict[int, dict[str, list[tuple[float, float]]]] = {}
    for step, side, _level, price, size in read_l2_rows(path):
        if price <= 0 or size <= 0:
            continue
        side_key = "bid" if side.lower() in {"bid", "b", "0"} else "ask"
        grouped.setdefault(step, {"bid": [], "ask": []})[side_key].append((price, size))

    rows = []
    for step in sorted(grouped):
        exch_ts = int(step * feed_latency_ns)
        local_ts = int(exch_ts + feed_latency_ns)
        bids = sorted(grouped[step]["bid"], reverse=True)
        asks = sorted(grouped[step]["ask"])
        for levels, side_event in (
            (bids, hft.BUY_EVENT),
            (asks, hft.SELL_EVENT),
        ):
            if not levels:
                continue
            clear_px = levels[-1][0]
            rows.append(
                (
                    hft.DEPTH_CLEAR_EVENT
                    | side_event
                    | hft.EXCH_EVENT
                    | hft.LOCAL_EVENT,
                    exch_ts,
                    local_ts,
                    clear_px,
                    0.0,
                    0,
                    0,
                    0.0,
                )
            )
            for price, size in levels:
                rows.append(
                    (
                        hft.DEPTH_SNAPSHOT_EVENT
                        | side_event
                        | hft.EXCH_EVENT
                        | hft.LOCAL_EVENT,
                        exch_ts,
                        local_ts,
                        price,
                        size,
                        0,
                        0,
                        0.0,
                    )
                )

    if not rows:
        raise SystemExit(f"no valid L2 rows in {path}")
    return np.asarray(rows, dtype=hft.event_dtype)


def convert_l2_to_hftbacktest(path: Path, feed_latency_ns: int) -> np.ndarray:
    try:
        import hftbacktest as hft  # type: ignore
    except ImportError:
        return legacy_event_rows(path, feed_latency_ns)
    if hasattr(hft, "event_dtype") and hasattr(hft, "BacktestAsset"):
        return structured_event_rows(path, hft, feed_latency_ns)
    return legacy_event_rows(path, feed_latency_ns)


def import_hftbacktest():
    try:
        import hftbacktest as hft  # type: ignore
    except ImportError as exc:
        raise RuntimeError("Install hftbacktest first: pip install hftbacktest") from exc

    if hasattr(hft, "BacktestAsset") and hasattr(hft, "HashMapMarketDepthBacktest"):
        return {
            "mode": "v2",
            "BacktestAsset": hft.BacktestAsset,
            "HashMapMarketDepthBacktest": hft.HashMapMarketDepthBacktest,
            "GTC": hft.GTC,
            "LIMIT": hft.LIMIT,
            "MARKET": hft.MARKET,
            "FILLED": hft.FILLED,
        }

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
        "mode": "legacy",
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


def run_v2_sell_window_backtest(
    args: argparse.Namespace,
    hft: dict,
    symbol: str,
    data: np.ndarray,
    entry_price: float,
) -> dict:
    sell_limit = entry_price * (1.0 + args.target_profit_pct) + args.cost_per_share
    asset = (
        hft["BacktestAsset"]()
        .data(data)
        .linear_asset(1.0)
        .constant_order_latency(args.order_latency_ns, args.order_latency_ns)
        .risk_adverse_queue_model()
        .no_partial_fill_exchange()
        .trading_value_fee_model(args.maker_fee, args.taker_fee)
        .tick_size(args.tick_size)
        .lot_size(args.lot_size)
        .last_trades_capacity(0)
    )
    hbt = hft["HashMapMarketDepthBacktest"]([asset])

    buy_order_id = 1
    sell_order_id = 2
    buy_submitted = False
    sell_submitted = False
    sell_filled = False
    last_mid = entry_price
    steps = 0

    try:
        while hbt.elapse(args.step_ns) == 0:
            steps += 1
            hbt.clear_inactive_orders(0)
            depth = hbt.depth(0)
            if depth.best_bid > 0 and depth.best_ask > 0:
                last_mid = 0.5 * (float(depth.best_bid) + float(depth.best_ask))

            if not buy_submitted:
                buy_price = float(depth.best_ask) if depth.best_ask > 0 else entry_price
                hbt.submit_buy_order(
                    0,
                    buy_order_id,
                    buy_price,
                    args.qty,
                    hft["GTC"],
                    hft["MARKET"],
                    False,
                )
                hbt.wait_order_response(0, buy_order_id, args.order_latency_ns * 10)
                buy_submitted = True

            if buy_submitted and not sell_submitted and hbt.position(0) >= args.qty:
                hbt.submit_sell_order(
                    0,
                    sell_order_id,
                    sell_limit,
                    args.qty,
                    hft["GTC"],
                    hft["LIMIT"],
                    False,
                )
                hbt.wait_order_response(0, sell_order_id, args.order_latency_ns * 10)
                sell_submitted = True

            if sell_submitted and hbt.position(0) <= 0:
                sell_filled = True
                break

        position = float(hbt.position(0))
        state_values = hbt.state_values(0)
        balance = float(state_values.balance)
        fee = float(state_values.fee)
        equity = balance + position * last_mid - fee
        return {
            "symbol": symbol,
            "entry_price": entry_price,
            "sell_limit": sell_limit,
            "qty": args.qty,
            "filled": sell_filled,
            "final_position": position,
            "balance": balance,
            "fee": fee,
            "equity": equity,
            "hftbacktest_mode": "v2_market_buy_then_limit_sell",
            "hftbacktest_steps": steps,
        }
    finally:
        hbt.close()


def run_sell_window_backtest(
    args: argparse.Namespace,
    symbol: str,
    data: np.ndarray,
    entry_price: float,
) -> dict:
    hft = import_hftbacktest()
    if hft["mode"] == "v2":
        return run_v2_sell_window_backtest(args, hft, symbol, data, entry_price)

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
        "symbol": symbol,
        "entry_price": entry_price,
        "sell_limit": sell_limit,
        "qty": args.qty,
        "filled": filled,
        "final_position": float(hbt.position),
        "balance": float(hbt.balance),
        "equity": float(hbt.equity),
    }


def run_symbol(args: argparse.Namespace, symbol: str) -> dict:
    cache = Path(args.cache_dir)
    symbol_file = safe_symbol(symbol)
    l1_csv = cache / f"{symbol_file}.mbp1.csv"
    l2_csv = cache / f"{symbol_file}.mbp10.csv"
    hft_npy = cache / f"{symbol_file}.hftbacktest.npy"

    ensure_data(
        args,
        symbol,
        l1_csv,
        args.l1_script,
        args.l1_dataset,
        "mbp-1",
        args.l1_chunk_hours,
    )
    entry_step, entry_price = read_l1_entry(l1_csv)
    ensure_data(
        args,
        symbol,
        l2_csv,
        args.l2_script,
        args.l2_dataset,
        "mbp-10",
        args.l2_chunk_hours,
    )
    data = convert_l2_to_hftbacktest(l2_csv, args.feed_latency_ns)
    hft_npy.parent.mkdir(parents=True, exist_ok=True)
    np.save(hft_npy, data)

    sell_limit = entry_price * (1.0 + args.target_profit_pct) + args.cost_per_share
    summary = {
        "symbol": symbol,
        "entry_step": entry_step,
        "entry_price": entry_price,
        "sell_limit": sell_limit,
        "l1_csv": str(l1_csv),
        "l2_csv": str(l2_csv),
        "hftbacktest_data": str(hft_npy),
    }
    summary.update(collect_l2_metrics(l2_csv, entry_price, sell_limit))
    if not args.prepare_only:
        try:
            summary.update(run_sell_window_backtest(args, symbol, data, entry_price))
            summary["hftbacktest_error"] = ""
        except Exception as exc:
            summary["hftbacktest_error"] = str(exc)

    return summary


def write_report(args: argparse.Namespace, summaries: list[dict]) -> str:
    filled = sum(1 for item in summaries if item.get("filled") is True)
    possible = sum(
        1 for item in summaries if item.get("target_fill_possible_from_best_bid")
    )
    errors = [item for item in summaries if item.get("hftbacktest_error")]

    lines = [
        "# Databento Backtest Report",
        "",
        f"- Period: `{args.start}` to `{args.end}`",
        f"- Symbols: `{', '.join(item['symbol'] for item in summaries)}`",
        f"- L1 dataset/schema: `{args.l1_dataset}` / `mbp-1`",
        f"- L2 dataset/schema: `{args.l2_dataset}` / `mbp-10`",
        f"- L1 chunk size: `{args.l1_chunk_hours}` hours",
        f"- L2 chunk size: `{args.l2_chunk_hours}` hours",
        f"- Target profit: `{args.target_profit_pct:.4%}`",
        f"- Cost per share: `{args.cost_per_share:.6f}`",
        f"- hftbacktest filled count: `{filled}/{len(summaries)}`",
        f"- Best-bid target-touch count: `{possible}/{len(summaries)}`",
        "",
        "| Symbol | Entry | Sell target | L2 steps | Avg spread bps | Target touched | hft filled | Equity | Error |",
        "|---|---:|---:|---:|---:|---|---|---:|---|",
    ]
    for item in summaries:
        error = item.get("hftbacktest_error", "")
        short_error = error.replace("|", "/")[:80]
        equity = item.get("equity", 0.0)
        lines.append(
            "| {symbol} | {entry:.4f} | {target:.4f} | {steps} | {spread:.2f} | {touch} | {filled} | {equity:.4f} | {error} |".format(
                symbol=item["symbol"],
                entry=float(item.get("entry_price", 0.0)),
                target=float(item.get("sell_limit", 0.0)),
                steps=int(item.get("l2_steps", 0)),
                spread=float(item.get("avg_spread_bps", 0.0)),
                touch="yes"
                if item.get("target_fill_possible_from_best_bid")
                else "no",
                filled="yes" if item.get("filled") is True else "no",
                equity=float(equity) if equity is not None else 0.0,
                error=short_error,
            )
        )

    if errors:
        lines.extend(
            [
                "",
                "## hftbacktest Errors",
                "",
                "The CSV download/metric path completed, but at least one symbol could not complete the hftbacktest engine run.",
            ]
        )

    text = "\n".join(lines) + "\n"
    report_path = Path(args.report)
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(text, encoding="utf-8")
    return str(report_path)


def main() -> int:
    args = parse_args()
    symbols = parse_symbols(args)
    summaries = [run_symbol(args, symbol) for symbol in symbols]

    report_path = write_report(args, summaries)
    summary = {"report": report_path, "results": summaries}
    text = json.dumps(summary, indent=2, sort_keys=True)
    if args.summary:
        Path(args.summary).write_text(text + "\n", encoding="utf-8")
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
