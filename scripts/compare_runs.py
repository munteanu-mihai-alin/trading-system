#!/usr/bin/env python3
"""Side-by-side comparison of backtest run metrics.

Reads metrics.json from each `reports/runs/<run_id>/` folder and prints
a markdown table with the headline numbers. Saves the table to
`reports/runs/_compare.md` for sharing.

Usage:
    scripts/compare_runs.py                    # all runs, latest 8
    scripts/compare_runs.py <id> <id> ...      # explicit list
    scripts/compare_runs.py --label-contains v # filter by run_id substring

Useful columns chosen for triage:
    n_trips, realized_pnl, win_rate, avg_per_trade,
    open_notional ($), open_count,
    avg_hold_min, sharpe_annualized, net_after_opp_cost,
    deepest_dd_pct (the honest-loss bag-holding figure)

The compute_metrics() function in plot_run.py is the source of truth
for what's in metrics.json. If a key isn't present in an older run's
metrics.json (because plot_run.py grew the field after the run), the
cell shows "-".
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple


# Column key -> (header, format string, source field in metrics.json).
# Derived columns (avg_per_trade) are computed from other keys below.
COLUMNS: List[Tuple[str, str, str]] = [
    ("n_trips",          "n_trips",          "n_round_trips_closed"),
    ("realized_pnl",     "realized_pnl",     "realized_pnl_net"),
    ("win_rate",         "win_rate",         "win_rate"),
    ("avg_per_trade",    "avg_$",            "_derived_avg_per_trade"),
    ("open_count",       "open_n",           "n_positions_open_at_end"),
    ("avg_hold_min",     "avg_hold_min",     "avg_holding_minutes"),
    ("sharpe",           "sharpe_y",         "sharpe_ratio_annualized"),
    ("net_after_opp",    "net_after_opp",    "net_pnl_after_opportunity_cost"),
    ("unrealized",       "unrealized",       "unrealized_pnl_mark_to_market"),
    ("deepest_dd_pct",   "dd%",              "deepest_drawdown_pct_any_position"),
]


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("run_ids", nargs="*",
                   help="Specific run IDs to compare. If empty, pick the latest 8.")
    p.add_argument("--runs-root", type=Path, default=Path("reports/runs"),
                   help="Where reports/runs/<run_id>/ folders live.")
    p.add_argument("--label-contains", default="",
                   help="Filter run_id by substring (e.g. 'v' to skip the older 'cpp_backtest' label).")
    p.add_argument("--limit", type=int, default=8,
                   help="Max runs when listing (default 8). Ignored when explicit ids given.")
    p.add_argument("--out", type=Path,
                   help="Write the markdown table here in addition to stdout. "
                        "Default: <runs-root>/_compare.md")
    return p.parse_args()


def discover_runs(runs_root: Path, label_contains: str, limit: int) -> List[Path]:
    """Returns up to `limit` run folders, newest first (by folder name).

    Run folder names are <YYYY-MM-DDTHHMM>_<label> so a name sort IS a
    time sort.
    """
    if not runs_root.is_dir():
        print(f"compare_runs: not a directory: {runs_root}", file=sys.stderr)
        return []
    folders = []
    for child in runs_root.iterdir():
        if not child.is_dir():
            continue
        if label_contains and label_contains not in child.name:
            continue
        if not (child / "metrics.json").exists():
            continue
        folders.append(child)
    folders.sort(key=lambda p: p.name, reverse=True)
    return folders[:limit]


def derived(metrics: Dict[str, Any], key: str) -> Optional[float]:
    if key == "_derived_avg_per_trade":
        n = metrics.get("n_round_trips_closed", 0) or 0
        pnl = metrics.get("realized_pnl_net", 0.0) or 0.0
        return pnl / n if n else None
    return None


def fmt_cell(value: Any, header: str) -> str:
    if value is None:
        return "-"
    if isinstance(value, bool):
        return str(value)
    if isinstance(value, (int, float)):
        if header in ("n_trips", "open_n"):
            return str(int(value))
        # win_rate as percent.
        if header == "win_rate":
            return f"{value * 100:.0f}%"
        # Hold in minutes; show with one decimal for under-hour values.
        if header == "avg_hold_min":
            return f"{value:.1f}"
        if header == "sharpe_y":
            return f"{value:.2f}"
        if header == "dd%":
            return f"{value:+.2f}"
        # Dollars - 2 decimals.
        return f"{value:+.2f}" if header.startswith("$") or "pnl" in header.lower() else f"{value:.2f}"
    return str(value)


def main(argv: List[str]) -> int:
    args = parse_args(argv) if False else __import__("argparse").Namespace()  # placeholder
    # Cleaner: just call parse_args() directly.
    args = parse_args()

    if args.run_ids:
        run_dirs = [args.runs_root / rid for rid in args.run_ids]
        for d in run_dirs:
            if not (d / "metrics.json").exists():
                print(f"compare_runs: missing metrics.json: {d}", file=sys.stderr)
                return 2
    else:
        run_dirs = discover_runs(args.runs_root, args.label_contains, args.limit)
        if not run_dirs:
            print(f"compare_runs: no runs found under {args.runs_root}", file=sys.stderr)
            return 1

    rows: List[Tuple[str, Dict[str, Any]]] = []
    for d in run_dirs:
        with (d / "metrics.json").open() as f:
            rows.append((d.name, json.load(f)))

    # Build the markdown table. First col is the run_id (truncated for
    # readability), one col per COLUMNS entry.
    headers = ["run"] + [c[1] for c in COLUMNS]
    lines = []
    lines.append("| " + " | ".join(headers) + " |")
    lines.append("|" + "|".join(["---"] * len(headers)) + "|")
    for run_id, m in rows:
        # Truncate "2026-05-25T0442_cpp_backtest_10day_v6_cachefix2" -> readable.
        short = run_id
        if len(short) > 50:
            short = short[:47] + "..."
        cells = [short]
        for col_key, header, source in COLUMNS:
            if source.startswith("_derived"):
                v = derived(m, source)
            else:
                v = m.get(source)
            cells.append(fmt_cell(v, header))
        lines.append("| " + " | ".join(cells) + " |")
    table = "\n".join(lines)

    print(table)

    out_path = args.out or (args.runs_root / "_compare.md")
    try:
        out_path.write_text(
            f"# Backtest run comparison\n\n{table}\n\n"
            f"_Generated by scripts/compare_runs.py on {len(rows)} runs._\n",
            encoding="utf-8",
        )
        print(f"\n(saved to {out_path})", file=sys.stderr)
    except Exception as exc:
        print(f"compare_runs: could not write {out_path}: {exc}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
