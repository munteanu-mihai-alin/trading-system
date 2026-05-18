#!/usr/bin/env python3
"""Export historical L1 (top-of-book) bid/ask bars from IBKR via TWS API.

Writes CSV in the same format the backtest expects:
    step,bid_price,bid_size,ask_price,ask_size

`step` is a 0-based row index in time order. `bid_size` and `ask_size` are
written as 0 because `whatToShow=BID_ASK` historical bars do not include
quote sizes; this script does not attempt to synthesize them. The bar's
time-averaged bid is taken from `open` and time-averaged ask from `close`,
per TWS API docs for the BID_ASK schema.

Pacing (per IBKR docs):
  - max 60 historical-data requests in any rolling 10-min window
  - BID_ASK counts as 2 against that cap (=> 30 BID_ASK / 10 min)
  - no identical request within 15 s

This script enforces a conservative 21 s sleep between requests, which keeps
us comfortably under the rolling cap regardless of bar size choice.

Usage (typical):
    python scripts/ibkr_historical_l1.py \\
        --symbol AAPL \\
        --start  2026-04-13 \\
        --end    2026-05-01 \\
        --output data/l1/AAPL.mbp1.csv \\
        --bar-size "1 min"

IB Gateway must be running and the connection port/clientId must be free.
Defaults assume paper IB Gateway on 127.0.0.1:4002.
"""

from __future__ import annotations

import argparse
import csv
import os
import queue
import sys
import threading
import time
from collections import deque
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import List

try:
    from ibapi.client import EClient
    from ibapi.contract import Contract
    from ibapi.wrapper import EWrapper
    from ibapi.common import BarData
except ImportError as exc:  # pragma: no cover - install hint
    raise SystemExit(
        "ibapi is required: pip install ibapi  (or use the IBKR-provided wheel)"
    ) from exc


# ---------------------------------------------------------------------------
# Per-bar-size lookback caps (the most useful slice of the IBKR table).
# Map bar size -> max `durationStr` accepted by reqHistoricalData.
# We chunk requests against these caps so the script does not need to know
# how IBKR will reject an over-long request.
# ---------------------------------------------------------------------------
DURATION_FOR_BAR = {
    "1 secs":   "1800 S",    # 30 min
    "5 secs":   "7200 S",    # 2 h   (docs: up to 2000 S in some places; 7200 OK in practice)
    "10 secs":  "14400 S",   # 4 h
    "15 secs":  "14400 S",
    "30 secs":  "28800 S",   # 8 h - covers an RTH day
    "1 min":    "1 D",
    "2 mins":   "2 D",
    "3 mins":   "1 W",
    "5 mins":   "1 W",
    "15 mins":  "2 W",
    "30 mins":  "1 M",
    "1 hour":   "1 M",
    "1 day":    "1 Y",
}

PACING_SLEEP_SECONDS = 21.0
PACING_WINDOW_SECONDS = 600.0
PACING_MAX_BIDASK_PER_WINDOW = 30


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser()
    p.add_argument("--symbol", required=True, help="Ticker symbol, e.g. AAPL")
    p.add_argument("--sec-type", default="STK")
    p.add_argument("--exchange", default="SMART")
    p.add_argument("--primary-exchange", default="NASDAQ",
                   help="Primary listing (NYSE, NASDAQ, ARCA, ...) for SMART routing.")
    p.add_argument("--currency", default="USD")
    p.add_argument("--start", required=True, help="UTC start date (YYYY-MM-DD or ISO).")
    p.add_argument("--end", required=True, help="UTC end date (YYYY-MM-DD or ISO).")
    p.add_argument("--bar-size", default="1 min",
                   help='IBKR barSizeSetting, e.g. "5 secs", "1 min", "1 hour".')
    p.add_argument("--use-rth", type=int, default=1,
                   help="1 = regular trading hours only; 0 = include pre/post.")
    p.add_argument("--output", required=True, help="Output CSV path.")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=int(os.environ.get("IBKR_PORT", 4002)))
    p.add_argument("--client-id", type=int, default=int(os.environ.get("IBKR_CLIENT_ID", 42)))
    p.add_argument("--timeout", type=float, default=60.0,
                   help="Per-request timeout in seconds.")
    p.add_argument("--connect-timeout", type=float, default=10.0)
    return p.parse_args()


def parse_dt(text: str) -> datetime:
    """Parse YYYY-MM-DD or full ISO-8601 (with or without trailing Z)."""
    text = text.strip()
    if text.endswith("Z"):
        text = text[:-1] + "+00:00"
    if len(text) == 10 and text[4] == "-" and text[7] == "-":
        return datetime.fromisoformat(text).replace(tzinfo=timezone.utc)
    dt = datetime.fromisoformat(text)
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return dt.astimezone(timezone.utc)


def duration_to_timedelta(duration: str) -> timedelta:
    """Convert an IBKR durationStr like '1 D', '7200 S' to a timedelta."""
    n_str, unit = duration.split()
    n = int(n_str)
    unit = unit.upper()
    if unit == "S":
        return timedelta(seconds=n)
    if unit == "D":
        return timedelta(days=n)
    if unit == "W":
        return timedelta(weeks=n)
    if unit == "M":
        return timedelta(days=30 * n)
    if unit == "Y":
        return timedelta(days=365 * n)
    raise ValueError(f"unsupported duration unit: {duration!r}")


# ---------------------------------------------------------------------------
# IBKR client - one in-flight historical request at a time.
# ---------------------------------------------------------------------------
@dataclass
class HistoricalResult:
    bars: List[BarData]
    error: str | None = None


class IBKRHistoricalClient(EWrapper, EClient):
    def __init__(self) -> None:
        EClient.__init__(self, wrapper=self)
        self._next_req_id = 1
        self._lock = threading.Lock()
        self._pending: dict[int, queue.Queue] = {}
        self._bars: dict[int, List[BarData]] = {}
        self._connected_event = threading.Event()
        self._next_valid_id_event = threading.Event()

    # ---- bookkeeping ----------------------------------------------------
    def next_req_id(self) -> int:
        with self._lock:
            rid = self._next_req_id
            self._next_req_id += 1
            return rid

    # ---- EWrapper overrides ---------------------------------------------
    def nextValidId(self, orderId: int) -> None:  # noqa: N802
        super().nextValidId(orderId)
        self._next_valid_id_event.set()
        self._connected_event.set()

    def historicalData(self, reqId: int, bar: BarData) -> None:  # noqa: N802
        self._bars.setdefault(reqId, []).append(bar)

    def historicalDataEnd(self, reqId: int, start: str, end: str) -> None:  # noqa: N802
        bars = self._bars.pop(reqId, [])
        q = self._pending.pop(reqId, None)
        if q is not None:
            q.put(HistoricalResult(bars=bars))

    # The ibapi error() callback has two signatures across versions:
    #   ibapi 9.81 (PyPI):    error(reqId, errorCode, errorString, advancedOrderRejectJson="")
    #   ibapi 10.x  (TWS API): error(reqId, errorTime, errorCode, errorString, advancedOrderRejectJson="")
    # Accept *args and split based on the type of the 2nd positional arg so
    # the script works with either library version.
    def error(self, reqId: int, *args, **kwargs) -> None:  # noqa: N802
        # Resolve overload: int second arg is the errorTime path (10.x);
        # str second arg is the errorString path (9.81).
        if len(args) >= 2 and isinstance(args[0], int) and isinstance(args[1], int):
            # 10.x: (errorTime, errorCode, errorString, advancedOrderRejectJson)
            errorCode = args[1]
            errorString = args[2] if len(args) >= 3 else ""
        else:
            # 9.81: (errorCode, errorString, advancedOrderRejectJson)
            errorCode = args[0] if args else 0
            errorString = args[1] if len(args) >= 2 else ""
        # Many "errors" from IBKR are informational (codes 2104, 2106, 2158 etc).
        # Only fail the request on real error codes (>=10000 or known pacing 162).
        is_info = errorCode in (2104, 2106, 2107, 2108, 2158, 2169, 2174)
        if is_info:
            return
        msg = f"[{errorCode}] {errorString}"
        if reqId in self._pending:
            self._bars.pop(reqId, None)
            self._pending[reqId].put(HistoricalResult(bars=[], error=msg))
            self._pending.pop(reqId, None)
        else:
            print(f"IBKR error: {msg}", file=sys.stderr)

    # ---- request helpers -------------------------------------------------
    def request_bid_ask(self, contract: Contract, end_dt: datetime,
                        duration: str, bar_size: str, use_rth: int,
                        timeout: float) -> HistoricalResult:
        rid = self.next_req_id()
        q: queue.Queue = queue.Queue(maxsize=1)
        self._pending[rid] = q
        end_str = end_dt.strftime("%Y%m%d-%H:%M:%S")
        self.reqHistoricalData(
            reqId=rid,
            contract=contract,
            endDateTime=end_str,
            durationStr=duration,
            barSizeSetting=bar_size,
            whatToShow="BID_ASK",
            useRTH=use_rth,
            formatDate=2,            # 2 = epoch seconds UTC
            keepUpToDate=False,
            chartOptions=[],
        )
        try:
            return q.get(timeout=timeout)
        except queue.Empty:
            self.cancelHistoricalData(rid)
            self._pending.pop(rid, None)
            self._bars.pop(rid, None)
            return HistoricalResult(bars=[], error="request timeout")


# ---------------------------------------------------------------------------
# Pacing limiter: track BID_ASK request times in a rolling 10-min deque, sleep
# whenever we'd exceed the cap. Also enforces a hard floor between requests.
# ---------------------------------------------------------------------------
class PacingGate:
    def __init__(self) -> None:
        self._stamps: deque[float] = deque()

    def wait_then_record(self) -> None:
        now = time.monotonic()
        while self._stamps and now - self._stamps[0] > PACING_WINDOW_SECONDS:
            self._stamps.popleft()
        if len(self._stamps) >= PACING_MAX_BIDASK_PER_WINDOW:
            sleep_for = PACING_WINDOW_SECONDS - (now - self._stamps[0]) + 1.0
            if sleep_for > 0:
                time.sleep(sleep_for)
        if self._stamps:
            since_last = time.monotonic() - self._stamps[-1]
            if since_last < PACING_SLEEP_SECONDS:
                time.sleep(PACING_SLEEP_SECONDS - since_last)
        self._stamps.append(time.monotonic())


# ---------------------------------------------------------------------------
# Main download loop.
# ---------------------------------------------------------------------------
def main() -> int:
    args = parse_args()

    if args.bar_size not in DURATION_FOR_BAR:
        raise SystemExit(
            f"unsupported --bar-size {args.bar_size!r}. "
            f"choose one of: {', '.join(DURATION_FOR_BAR)}"
        )

    start_dt = parse_dt(args.start)
    end_dt = parse_dt(args.end)
    if end_dt <= start_dt:
        raise SystemExit("--end must be after --start")

    duration = DURATION_FOR_BAR[args.bar_size]
    chunk_td = duration_to_timedelta(duration)

    contract = Contract()
    contract.symbol = args.symbol
    contract.secType = args.sec_type
    contract.exchange = args.exchange
    contract.primaryExchange = args.primary_exchange
    contract.currency = args.currency

    client = IBKRHistoricalClient()
    client.connect(args.host, args.port, args.client_id)
    api_thread = threading.Thread(target=client.run, daemon=True)
    api_thread.start()
    if not client._connected_event.wait(args.connect_timeout):
        client.disconnect()
        raise SystemExit(
            f"could not connect to IB Gateway at {args.host}:{args.port} "
            f"(clientId={args.client_id}); is the gateway running and accepting API?"
        )

    pacing = PacingGate()
    all_bars: list[BarData] = []
    cursor_end = end_dt
    chunk_index = 0
    print(
        f"requesting BID_ASK bars: symbol={args.symbol} "
        f"{start_dt.isoformat()} -> {end_dt.isoformat()} "
        f"bar={args.bar_size!r} chunk={duration!r}",
        file=sys.stderr,
    )

    try:
        while cursor_end > start_dt:
            pacing.wait_then_record()
            chunk_start = max(start_dt, cursor_end - chunk_td)
            chunk_index += 1
            print(
                f"  chunk {chunk_index}: end={cursor_end.isoformat()} "
                f"(covers back to {chunk_start.isoformat()})",
                file=sys.stderr,
            )
            result = client.request_bid_ask(
                contract=contract,
                end_dt=cursor_end,
                duration=duration,
                bar_size=args.bar_size,
                use_rth=args.use_rth,
                timeout=args.timeout,
            )
            if result.error:
                print(f"  ! chunk failed: {result.error}", file=sys.stderr)
                # Step the cursor back anyway so we make progress; the gap will
                # show up as missing rows in the output. Pacing violations are
                # the main reason for fallthrough here.
                cursor_end = chunk_start
                continue
            if not result.bars:
                cursor_end = chunk_start
                continue
            all_bars.extend(result.bars)
            cursor_end = chunk_start
    finally:
        client.disconnect()

    # Bars arrive newest-batch-first across chunks; sort by epoch ts to be safe.
    def bar_ts(bar: BarData) -> int:
        try:
            return int(bar.date)
        except (TypeError, ValueError):
            return 0

    all_bars.sort(key=bar_ts)
    # Drop bars outside the requested window (chunk overshoot is common).
    start_epoch = int(start_dt.timestamp())
    end_epoch = int(end_dt.timestamp())
    bars = [b for b in all_bars if start_epoch <= bar_ts(b) <= end_epoch]
    # Deduplicate identical timestamps (chunk overlap edges).
    seen_ts: set[int] = set()
    unique: list[BarData] = []
    for b in bars:
        ts = bar_ts(b)
        if ts in seen_ts:
            continue
        seen_ts.add(ts)
        unique.append(b)

    out_path = Path(args.output)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(
            ["ts_event", "step", "bid_price", "bid_size", "ask_price", "ask_size"]
        )
        for step, bar in enumerate(unique):
            # whatToShow=BID_ASK schema:
            #   open  = time-average bid
            #   close = time-average ask
            #   low   = min bid, high = max ask  (not used here)
            #   volume = -1 (size not provided)
            ts_event_ns = bar_ts(bar) * 1_000_000_000
            writer.writerow([
                ts_event_ns,
                step,
                f"{bar.open:.6f}",
                0,
                f"{bar.close:.6f}",
                0,
            ])

    print(
        f"wrote {len(unique)} rows to {out_path} "
        f"(requested {chunk_index} chunks, kept {len(unique)}/{len(all_bars)} bars)",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
