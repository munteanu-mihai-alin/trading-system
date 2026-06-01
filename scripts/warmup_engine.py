#!/usr/bin/env python3
"""Generate a warmup_state file that the live engine reads at startup.

Backlog item 11. The live engine cold-starts with Hawkes lambda = 0,
OU mu = 0, hit_count = 0; the first ~30 min of trading produce no
useful scores until those accumulators warm up. This script runs an
offline pass over the last N hours of Databento L1 + L2 and writes
the warmed-up state to a small text file the C++ engine can read
via `AppConfig::warmup_state_path` + `seed_warmup_state_from_file`.

Output format (intentionally simple, no JSON to avoid wedging a JSON
parser into the engine):

    # produced 2026-06-01T13:25:00Z, window_hours=3
    produced_at_ns=1717249500000000000
    window_hours=3
    AAPL.hawkes_lambda=12.5
    AAPL.ou_mu=215.4
    AAPL.hit_count=0
    AAPL.ou_initialized=1
    NVDA.hawkes_lambda=8.7
    NVDA.ou_mu=108.2
    ...

Usage:
    scripts/warmup_engine.py --window-hours 3 --out warmup_state.txt
    scripts/warmup_engine.py --window-hours 3 --symbols config/symbols_yen.txt

NOTE: this script is currently a SKELETON. The actual Hawkes / OU
fitting against historical data needs to use the same models as
`include/models/{ou,hawkes}.hpp` -- mirror the update rules in
Python so the warmup numbers are comparable to what the engine would
compute live. That implementation is the next step on this thread;
the skeleton lays out the I/O contract so the engine side can be
tested independently.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path
from typing import Dict, List, Optional, Tuple


# Same default 50-symbol universe as
# scripts/ibkr_symbol_contract_probe.py.
DEFAULT_UNIVERSE = [
    "AAPL", "NVDA", "AMD", "INTC", "MU", "QCOM", "ARM", "ASML", "AMAT",
    "LRCX", "KLAC", "SNPS", "CDNS", "MKSI", "ENTG", "STX", "WDC", "PSTG",
    "DELL", "HPQ", "SMCI", "CSCO", "HPE", "IBM", "KEYS", "TSM", "GFS",
    "UMC", "TSEM", "ASX", "AMKR", "IMOS", "LEA", "AWK", "CEG", "VST",
    "NIO", "XPEV", "OKLO", "SNDK", "LMT", "HWM", "RTX", "NOC", "GSM",
    "DD", "LIN", "APD", "TTE", "NOK",
]


def load_symbols(symbols_arg: Optional[Path]) -> List[str]:
    if symbols_arg is None:
        return list(DEFAULT_UNIVERSE)
    if not symbols_arg.is_file():
        raise SystemExit(f"symbols file not found: {symbols_arg}")
    out: List[str] = []
    for line in symbols_arg.read_text().splitlines():
        t = line.strip()
        if not t or t.startswith("#"):
            continue
        sym = t.split(",", 1)[0].strip()
        if sym:
            out.append(sym)
    return out


def compute_warmup_state(
    symbols: List[str],
    window_hours: int,
) -> Dict[str, Dict[str, float]]:
    """Compute warmed-up Hawkes / OU / hit_count per symbol.

    PLACEHOLDER -- the real implementation walks the last N hours
    of historical L1 trade prints from Databento, drives the same
    update rules as include/models/{hawkes,ou}.hpp, and returns
    the converged values.

    For now we emit defensible neutral values: lambda = 1.0 (engine
    default mu), ou_mu = NaN-not-initialized (so the engine treats
    them as ou_initialized=false and proceeds cold), hit_count = 0.
    The caller still benefits from the engine side being WIRED;
    they just won't see a benefit until the real fit is implemented.
    """
    out: Dict[str, Dict[str, float]] = {}
    for sym in symbols:
        out[sym] = {
            "hawkes_lambda": 1.0,
            "ou_mu": 0.0,
            "hit_count": 0.0,
            "ou_initialized": 0.0,  # not initialized until real fit
        }
    # TODO: hook Databento. Real implementation:
    #   1. Pull <window_hours> of L1 + trade prints via databento.Historical.
    #   2. For each symbol, replay the prints through OUState::update +
    #      Hawkes::update with the same parameters as AppConfig sets
    #      live.
    #   3. Snapshot the final lambda / mu / hit_count.
    return out


def write_warmup_file(
    out_path: Path,
    state: Dict[str, Dict[str, float]],
    window_hours: int,
) -> None:
    produced_at_ns = int(time.time_ns())
    lines: List[str] = [
        f"# produced {time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} "
        f"window_hours={window_hours}",
        f"produced_at_ns={produced_at_ns}",
        f"window_hours={window_hours}",
    ]
    for sym in sorted(state.keys()):
        s = state[sym]
        lines.append(f"{sym}.hawkes_lambda={s['hawkes_lambda']:.6f}")
        lines.append(f"{sym}.ou_mu={s['ou_mu']:.6f}")
        lines.append(f"{sym}.hit_count={int(s['hit_count'])}")
        lines.append(f"{sym}.ou_initialized={int(s['ou_initialized'])}")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main(argv: List[str]) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--window-hours", type=int, default=3,
                   help="How many hours of history to warm up from.")
    p.add_argument("--symbols", type=Path,
                   help="symbols_*.txt to scope the warmup. "
                        "Default = 50-symbol baseline.")
    p.add_argument("--out", type=Path, default=Path("warmup_state.txt"))
    args = p.parse_args(argv)

    symbols = load_symbols(args.symbols)
    print(f"warmup: window={args.window_hours}h, "
          f"symbols={len(symbols)}, out={args.out}", file=sys.stderr)
    state = compute_warmup_state(symbols, args.window_hours)
    write_warmup_file(args.out, state, args.window_hours)
    print(f"warmup: wrote {len(state)} symbols to {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
