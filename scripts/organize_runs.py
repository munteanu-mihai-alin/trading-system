#!/usr/bin/env python3
"""Organize backtest artifacts under reports/runs/<run_id>/ with a manifest.

Reads loose files in reports/ (cpp_backtest_stdout.log, decisions.csv,
oneday_aapl_*, databento_*) and the project config.ini, groups them by
known harness conventions, and writes:

    reports/runs/<YYYY-MM-DDTHHMM>_<label>/
        manifest.json   - metadata (period, config, outcome, notes)
        stdout.log      - the run's stdout (when present)
        decisions.csv   - decision trace (when present)
        report.md       - any markdown report (when present)
        summary.json    - any JSON summary (when present)
        config.ini      - snapshot of the config used (when reachable)

Also rewrites reports/runs/index.md as a table of all known runs.

This is intentionally idempotent: re-running picks up new files and
leaves existing per-run folders untouched.

Usage:
    python3 scripts/organize_runs.py [--reports REPORTS_DIR] [--config CONFIG]
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser()
    p.add_argument("--reports", default="reports",
                   help="reports root (default: reports)")
    p.add_argument("--config",  default="config.ini",
                   help="optional config.ini to snapshot (default: config.ini)")
    return p.parse_args()


def parse_log_first_iso(path: Path) -> Optional[str]:
    """Return the first ISO-ish timestamp found in the log file, or None."""
    try:
        with path.open("r", encoding="utf-8", errors="ignore") as f:
            for _ in range(50):
                line = f.readline()
                if not line:
                    break
                if line.startswith("["):
                    # spdlog pattern: [2026-05-18 02:21:09.563] ...
                    bracket = line.split("]", 1)[0].lstrip("[")
                    return bracket.replace(" ", "T")
        return None
    except OSError:
        return None


def parse_log_engine_stopped(path: Path) -> Optional[str]:
    """Return the timestamp of the last spdlog line, the rough end time."""
    last_ts = None
    try:
        with path.open("r", encoding="utf-8", errors="ignore") as f:
            for line in f:
                if line.startswith("["):
                    bracket = line.split("]", 1)[0].lstrip("[")
                    last_ts = bracket.replace(" ", "T")
        return last_ts
    except OSError:
        return None


def parse_log_end_summary(path: Path) -> dict:
    """Extract key end-of-run lines from the C++ stdout log."""
    out: dict = {}
    try:
        text = path.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return out
    for line in text.splitlines():
        if line.startswith("Latency (cycles):"):
            out["latency_line"] = line.strip()
        elif line.startswith("Validation:"):
            out["validation_line"] = line.strip()
        elif line.startswith("Mode:"):
            out["mode_line"] = line.strip()
        elif line.startswith("Orders placed (engine):"):
            try:
                out["orders_placed"] = int(line.split(":", 1)[1].strip())
            except (ValueError, IndexError):
                pass
        elif line.startswith("Open positions at end:"):
            try:
                out["open_positions"] = int(line.split(":", 1)[1].strip())
            except (ValueError, IndexError):
                pass
        elif line.startswith("Open notional at end:"):
            try:
                out["open_notional"] = float(line.split(":", 1)[1].strip())
            except (ValueError, IndexError):
                pass
    return out


def read_text_safe(path: Path, default: str = "") -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return default


def read_json_safe(path: Path) -> Optional[dict]:
    try:
        return json.loads(path.read_text(encoding="utf-8", errors="ignore"))
    except (OSError, json.JSONDecodeError):
        return None


def file_mtime_iso(path: Path) -> str:
    return datetime.fromtimestamp(path.stat().st_mtime, tz=timezone.utc) \
        .isoformat(timespec="seconds")


def run_folder_id(prefix_iso: str, label: str) -> str:
    """Convert "2026-05-18T02:21:09Z" -> "2026-05-18T0221_<label>".
    """
    if not prefix_iso:
        prefix = "unknown"
    else:
        head = prefix_iso.replace("Z", "")[:16]  # YYYY-MM-DDTHH:MM
        prefix = head.replace(":", "")
    return f"{prefix}_{label}"


def copy_if_present(src: Path, dst_dir: Path, dst_name: str) -> bool:
    if not src.exists():
        return False
    dst_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst_dir / dst_name)
    return True


def organize_cpp_backtest(reports: Path, runs: Path, config_path: Path) -> None:
    stdout_log = reports / "cpp_backtest_stdout.log"
    decisions  = reports / "decisions.csv"
    if not stdout_log.exists():
        return
    started_at = parse_log_first_iso(stdout_log) or file_mtime_iso(stdout_log)
    ended_at   = parse_log_engine_stopped(stdout_log) or started_at
    run_id = run_folder_id(started_at, "cpp_backtest")
    run_dir = runs / run_id
    if (run_dir / "manifest.json").exists():
        print(f"skip {run_id} (manifest exists)")
        return
    copy_if_present(stdout_log, run_dir, "stdout.log")
    copy_if_present(decisions,  run_dir, "decisions.csv")
    copy_if_present(config_path, run_dir, "config.ini")
    summary = parse_log_end_summary(stdout_log)
    manifest = {
        "run_id": run_id,
        "harness": "cpp_hft_app",
        "label": "C++ Databento backtest",
        "started_at": started_at + ("Z" if not started_at.endswith("Z") else ""),
        "ended_at":   ended_at   + ("Z" if not ended_at.endswith("Z") else ""),
        "outcome": "completed" if "orders_placed" in summary else "unknown",
        **{k: v for k, v in summary.items() if k != "latency_line"},
        "latency_line": summary.get("latency_line"),
        "artifacts": sorted(p.name for p in run_dir.iterdir()),
        "notes": ("Auto-generated from stdout. Inspect stdout.log for the "
                  "full end-of-run summary block."),
    }
    manifest["artifacts"].append("manifest.json")
    (run_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(f"wrote {run_id}")


def organize_python_pair(reports: Path, runs: Path, base: str,
                         label: str, harness: str = "python_run_hftbacktest_databento") -> None:
    """Group a (report.md, summary.json, stdout.log) trio with a shared
    `base` filename prefix (e.g., 'oneday_aapl' or 'databento_synthetic')."""
    report  = reports / f"{base}_report.md"
    summ    = reports / f"{base}_summary.json"
    stdout  = reports / f"{base}_stdout.log"
    if not (report.exists() or summ.exists()):
        return
    seed = stdout if stdout.exists() else (summ if summ.exists() else report)
    started_at = file_mtime_iso(seed)
    run_id = run_folder_id(started_at, f"python_{base}")
    run_dir = runs / run_id
    if (run_dir / "manifest.json").exists():
        print(f"skip {run_id} (manifest exists)")
        return
    copy_if_present(report, run_dir, "report.md")
    copy_if_present(summ,   run_dir, "summary.json")
    copy_if_present(stdout, run_dir, "stdout.log")
    summary_obj = read_json_safe(summ) if summ.exists() else None
    manifest = {
        "run_id": run_id,
        "harness": harness,
        "label": label,
        "started_at": started_at,
        "ended_at":   started_at,  # python runs are usually short; no end marker
        "outcome": "completed",
        "summary_excerpt": (summary_obj or {}).get("results", [{}])[0]
                           if isinstance(summary_obj, dict) else None,
        "artifacts": sorted(p.name for p in run_dir.iterdir()) + ["manifest.json"],
        "notes": "Auto-generated from file mtimes; check summary.json for exact run scope.",
    }
    (run_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(f"wrote {run_id}")


def rewrite_index(runs: Path) -> None:
    lines = [
        "# Backtest run index",
        "",
        f"Auto-generated {datetime.now(tz=timezone.utc).isoformat(timespec='seconds')}",
        "",
        "| Run ID | Harness | Started | Outcome | Label |",
        "|---|---|---|---|---|",
    ]
    rows: list[tuple[str, str, str, str, str]] = []
    for d in sorted(runs.iterdir()):
        if not d.is_dir():
            continue
        m = d / "manifest.json"
        if not m.exists():
            continue
        try:
            meta = json.loads(m.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            continue
        rows.append((
            meta.get("run_id", d.name),
            meta.get("harness", ""),
            meta.get("started_at", ""),
            meta.get("outcome", ""),
            meta.get("label", ""),
        ))
    rows.sort(key=lambda r: r[2])  # by started_at
    for r in rows:
        lines.append("| " + " | ".join(r) + " |")
    (runs / "index.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {runs / 'index.md'} ({len(rows)} rows)")


def main() -> int:
    args = parse_args()
    reports = Path(args.reports)
    runs = reports / "runs"
    runs.mkdir(parents=True, exist_ok=True)
    config_path = Path(args.config)

    organize_cpp_backtest(reports, runs, config_path)
    organize_python_pair(reports, runs, "oneday_aapl",
                         "1-day AAPL Databento backtest (Python hftbacktest)")
    organize_python_pair(reports, runs, "databento_synthetic_hourly",
                         "Hourly synthetic Databento smoke (Python)")
    organize_python_pair(reports, runs, "databento_real_smoke",
                         "1-hour AAPL real Databento smoke (Python)")
    organize_python_pair(reports, runs, "databento_synthetic",
                         "Synthetic Databento smoke (Python)")
    organize_python_pair(reports, runs, "databento_synthetic_v2",
                         "Synthetic v2 hftbacktest adapter smoke (Python)")
    rewrite_index(runs)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
