#!/usr/bin/env python3
"""Print Databento cost estimates for MBP-1 (L1) queries across our planned
backtest windows. No data is downloaded - this calls only the
metadata.get_cost endpoint, which is free.

Run on Hetzner where databento is installed and the API key is configured:
    .venv/bin/python scripts/databento_l1_cost_quote.py

Optional flags:
    --windows yen,covid,2026q2   limit to a subset (default: all three)
    --symbols-file PATH          override the symbol list for a given window

Output columns:
    window       Friendly name (yen / covid / 2026q2)
    schema       mbp-1 (we also re-quote mbp-10 as a sanity check)
    symbols      Number of symbols in the query
    duration     Days span (end - start) in calendar days
    cost_usd     Quote from databento.metadata.get_cost()

Cost lookup logic: feed the same parameters that databento_download_l2.py
would use for an actual download. The returned figure is a quote, not a
charge.
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

DEFAULT_API_KEY_FILE = Path.home() / ".config" / "trading-system" / "databento_api_key"

# Cross-window symbol lists. covid drops 5 post-window IPOs; yen drops SNDK.
WINDOWS = {
    "2026q2": {
        "start": "2026-04-13T13:30:00Z",
        "end":   "2026-04-29T00:00:00Z",
        "symbols_file": "config/symbols_yen.txt",  # 49 sym; close enough as proxy
    },
    "yen": {
        "start": "2024-08-02T13:30:00Z",
        "end":   "2024-08-09T20:00:00Z",
        "symbols_file": "config/symbols_yen.txt",
    },
    "covid": {
        "start": "2020-03-09T13:30:00Z",
        "end":   "2020-03-20T20:00:00Z",
        "symbols_file": "config/symbols_covid.txt",
    },
}


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument(
        "--windows",
        default=",".join(WINDOWS),
        help="Comma-separated subset of: " + ", ".join(WINDOWS),
    )
    p.add_argument(
        "--api-key-file",
        default=os.environ.get("DATABENTO_API_KEY_FILE", str(DEFAULT_API_KEY_FILE)),
    )
    p.add_argument(
        "--dataset", default="XNAS.ITCH",
        help="Databento dataset. XNAS.ITCH covers NASDAQ-listed symbols.",
    )
    p.add_argument(
        "--also-mbp10", action="store_true",
        help="Also quote MBP-10 for the same params (sanity check vs what we paid).",
    )
    return p.parse_args()


def read_api_key(path: str) -> str:
    p = Path(path).expanduser()
    if not p.exists():
        raise SystemExit(f"API key file not found: {p}")
    for line in p.read_text().splitlines():
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        if s.startswith("DATABENTO_API_KEY="):
            return s.split("=", 1)[1].strip().strip("'\"")
        return s.strip("'\"")
    raise SystemExit(f"API key file empty: {p}")


def load_symbols(path: str) -> list[str]:
    out = []
    for line in Path(path).read_text().splitlines():
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        out.append(s.split(",", 1)[0])
    return out


def main() -> int:
    args = parse_args()

    try:
        import databento as db  # type: ignore
    except ImportError:
        raise SystemExit("databento package not installed. Run on Hetzner.")

    os.environ["DATABENTO_API_KEY"] = read_api_key(args.api_key_file)
    client = db.Historical()

    print(f"{'window':10s} {'schema':8s} {'#sym':>5s} {'days':>5s} {'cost_usd':>10s}  notes")
    print("-" * 70)

    selected = [w.strip() for w in args.windows.split(",") if w.strip()]
    for name in selected:
        if name not in WINDOWS:
            print(f"unknown window: {name} (skipped)", file=sys.stderr)
            continue
        spec = WINDOWS[name]
        symbols = load_symbols(spec["symbols_file"])
        import datetime as dt
        days = (dt.datetime.fromisoformat(spec["end"].replace("Z", "+00:00"))
                - dt.datetime.fromisoformat(spec["start"].replace("Z", "+00:00"))).days

        for schema in ["mbp-1"] + (["mbp-10"] if args.also_mbp10 else []):
            try:
                cost = client.metadata.get_cost(
                    dataset=args.dataset,
                    symbols=symbols,
                    schema=schema,
                    start=spec["start"],
                    end=spec["end"],
                    mode="historical-streaming",
                )
            except Exception as exc:
                print(f"{name:10s} {schema:8s} {len(symbols):>5d} {days:>5d}  --        error: {exc}")
                continue
            print(f"{name:10s} {schema:8s} {len(symbols):>5d} {days:>5d}  ${cost:>9.2f}  {args.dataset}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
