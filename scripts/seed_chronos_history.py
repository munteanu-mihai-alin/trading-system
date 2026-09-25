#!/usr/bin/env python3
"""Seed the Chronos daily-close window from IBKR historical daily bars.

The window is the only input Chronos gets and it grows one close per
trading day, so an unseeded engine needs ~3 months of live running
before it can forecast anything. This writes a ready-made history.csv
in exactly the format Chronos2ExecutionEngine::load_daily_close_history
reads, so seeding needs no engine change at all.

IBKR returns SPLIT-ADJUSTED history: pre-split prices are retroactively
rescaled, so the series is continuous and already on today's price
scale -- the same scale the live mids the engine appends arrive on.
Raw (unadjusted) history would show a split as a huge fake drop, and
since the entry rule fires on predicted return >= target_profit_pct,
that artifact could manufacture a buy signal for a move that never
happened.

Known seams, deliberately not papered over:
  * Bars are TRADES closes; the engine appends (bid+ask)/2 mids. On
    wide-spread names the difference can be material (IMOS has traded
    22.51/23.74 -- a 5.3% spread).
  * The engine appends on the first step of a new day, which under the
    09:25 ET RTH timer is near the OPEN, not the close. So seeded
    closes and appended opens differ by one overnight gap at the join.
  * A split occurring AFTER seeding still breaks the window: the engine
    appends raw mids and never retro-adjusts stored history.

Usage:
  python3 seed_chronos_history.py --out <dir>/history.csv [--force]
"""
import argparse
import os
import sys
import time

NL = chr(10)

DEFAULT_OUT = ("/mnt/HC_Volume_105581071/trading-live/data/chronos2/"
               "daily_closes/history.csv")
DEFAULT_SYMBOLS = ("/mnt/HC_Volume_105581071/trading-live/services/config/"
                   "symbols_yen.txt")


def load_symbols(path):
    out = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            out.append(line.split(",", 1)[0].strip())
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--symbols", default=DEFAULT_SYMBOLS)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=4002,
                    help="4002 paper / 4001 live")
    ap.add_argument("--client-id", type=int, default=77,
                    help="must differ from the engine's client_id")
    ap.add_argument("--days", type=int, default=64,
                    help="trading days to keep (chronos2_context_len)")
    ap.add_argument("--duration", default="6 M",
                    help="IBKR lookback; needs slack for holidays")
    ap.add_argument("--what-to-show", default="TRADES",
                    help="TRADES (closes) or MIDPOINT (closer to engine mids)")
    ap.add_argument("--pace", type=float, default=1.5,
                    help="seconds between requests; IBKR paces historical data")
    ap.add_argument("--force", action="store_true",
                    help="overwrite an existing history.csv")
    args = ap.parse_args()

    # Refuse to clobber a window the engine has been accumulating. Real
    # history is expensive to rebuild and impossible to recover.
    if os.path.exists(args.out) and not args.force:
        print("refusing to overwrite existing %s (use --force)" % args.out)
        return 2

    try:
        from ib_insync import IB, Stock
    except ImportError:
        print("ib_insync not installed in this interpreter")
        return 1

    symbols = load_symbols(args.symbols)
    print("universe: %d symbols" % len(symbols))

    ib = IB()
    ib.connect(args.host, args.port, clientId=args.client_id, timeout=20)
    print("connected to %s:%d" % (args.host, args.port))

    rows = []
    ok, failed = [], []
    for i, sym in enumerate(symbols, 1):
        try:
            contract = Stock(sym, "SMART", "USD")
            ib.qualifyContracts(contract)
            bars = ib.reqHistoricalData(
                contract,
                endDateTime="",
                durationStr=args.duration,
                barSizeSetting="1 day",
                whatToShow=args.what_to_show,
                useRTH=True,
                formatDate=1,
            )
            if not bars or len(bars) < args.days:
                failed.append("%s(%d bars)" % (sym, len(bars) if bars else 0))
            else:
                for b in bars[-args.days:]:
                    rows.append((sym, str(b.date), float(b.close)))
                ok.append(sym)
        except Exception as exc:
            failed.append("%s(%s)" % (sym, str(exc)[:40]))
        print("  [%2d/%d] %-6s %s" % (i, len(symbols), sym,
                                      "ok" if sym in ok else "FAILED"))
        time.sleep(args.pace)

    ib.disconnect()

    if not rows:
        print("no data retrieved; nothing written")
        return 1

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    tmp = args.out + ".tmp"
    with open(tmp, "w") as f:
        f.write("symbol,date,close" + NL)
        for sym, date, close in rows:
            f.write("%s,%s,%.4f%s" % (sym, date, close, NL))
    os.replace(tmp, args.out)

    print("")
    print("wrote %s" % args.out)
    print("  %d rows, %d symbols x %d days" % (len(rows), len(ok), args.days))
    print("  size: %.1f KB" % (os.path.getsize(args.out) / 1024.0))
    if failed:
        print("  FAILED (%d): %s" % (len(failed), ", ".join(failed)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
