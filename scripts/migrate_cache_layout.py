#!/usr/bin/env python3
"""Move legacy flat-layout L1/L2 caches into the dated per-window layout.

Old layout (pre-migration):
    data/l1/AAPL.mbp1.csv
    data/databento/AAPL.mbp10.csv

New layout (post-migration):
    data/l1/<startDate>_<endDate>/AAPL_<startISO>_<endISO>.mbp1.csv
    data/l2/<startDate>_<endDate>/AAPL_<startISO>_<endISO>.mbp10.csv

Where:
    startDate / endDate are YYYY-MM-DD (UTC date of first / last ts_event)
    startISO / endISO  are YYYYMMDDTHHMMSSZ (basic ISO 8601, no colons)

Both date forms come from the file's actual first/last ts_event row.
Filename format mirrors include/broker/cache_filename.hpp so the broker
can find these via cross-folder glob without opening the file.

Refuses to operate on:
  - files lacking a 'ts_event' header (legacy CSVs from before that column
    was added; those files have no recoverable wall-clock and would need
    a re-download anyway)
  - files in an empty state or with unreadable timestamps

Idempotent: re-running on a tree that is already migrated is a no-op,
because the scanner only matches files in the top level of <old_root>
(legacy layout), not files inside any subfolder (new layout).

Default paths assume running from D:/trading-system; override with the
flags. For the Hetzner mirror, ssh in and run from the workdir there.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import shutil
import sys
from pathlib import Path


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument(
        "--l1-old-root",
        default="data/l1",
        help="Directory currently holding legacy <SYMBOL>.mbp1.csv files.",
    )
    p.add_argument(
        "--l1-new-root",
        default="data/l1",
        help="Directory to place dated <startDate>_<endDate>/<SYMBOL>_..mbp1.csv files. "
        "Same as --l1-old-root by default (in-place reorg).",
    )
    p.add_argument(
        "--l2-old-root",
        default="data/databento",
        help="Directory currently holding legacy <SYMBOL>.mbp10.csv files.",
    )
    p.add_argument(
        "--l2-new-root",
        default="data/l2",
        help="Directory to place dated <startDate>_<endDate>/<SYMBOL>_..mbp10.csv files.",
    )
    p.add_argument(
        "--dry-run",
        action="store_true",
        help="Print planned moves without touching disk.",
    )
    return p.parse_args()


def read_ts_event_range(path: Path) -> tuple[int, int] | None:
    """Return (first_ts_ns, last_ts_ns) from the first and last data row.

    Returns None if the file is missing the ts_event column or has no
    parseable data rows.
    """
    with path.open("r", newline="", encoding="utf-8") as f:
        reader = csv.reader(f)
        try:
            header = next(reader)
        except StopIteration:
            return None
        if not header or header[0] != "ts_event":
            return None
        first_ts: int | None = None
        last_ts: int | None = None
        for row in reader:
            if not row:
                continue
            try:
                ts = int(row[0])
            except (ValueError, IndexError):
                continue
            if first_ts is None:
                first_ts = ts
            last_ts = ts
    if first_ts is None or last_ts is None:
        return None
    return (first_ts, last_ts)


def format_iso_compact(ts_ns: int) -> str:
    """YYYYMMDDTHHMMSSZ - matches include/broker/cache_filename.hpp."""
    when = dt.datetime.fromtimestamp(ts_ns / 1e9, tz=dt.timezone.utc)
    return when.strftime("%Y%m%dT%H%M%SZ")


def format_date(ts_ns: int) -> str:
    """YYYY-MM-DD UTC."""
    when = dt.datetime.fromtimestamp(ts_ns / 1e9, tz=dt.timezone.utc)
    return when.strftime("%Y-%m-%d")


def plan_move(src: Path, new_root: Path, suffix: str) -> Path | None:
    """Compute destination path for src under new_root, or None if skipped."""
    rng = read_ts_event_range(src)
    if rng is None:
        print(f"  skip {src.name}: no ts_event range (legacy schema or empty)",
              file=sys.stderr)
        return None
    start_ns, end_ns = rng
    if end_ns <= start_ns:
        print(f"  skip {src.name}: degenerate ts range ({start_ns}..{end_ns})",
              file=sys.stderr)
        return None
    symbol = src.name[: -len(suffix)]
    folder = f"{format_date(start_ns)}_{format_date(end_ns)}"
    leaf = f"{symbol}_{format_iso_compact(start_ns)}_{format_iso_compact(end_ns)}{suffix}"
    return new_root / folder / leaf


def migrate_directory(
    old_root: Path,
    new_root: Path,
    suffix: str,
    dry_run: bool,
) -> tuple[int, int]:
    """Scan old_root for *<suffix> at top level. Returns (moved, skipped)."""
    if not old_root.exists():
        print(f"== {old_root}: does not exist; nothing to migrate", file=sys.stderr)
        return (0, 0)
    moved = 0
    skipped = 0
    # Top-level only on purpose - files inside subdirs are already migrated
    # (or someone else's, leave them alone).
    candidates = sorted(
        p for p in old_root.iterdir() if p.is_file() and p.name.endswith(suffix)
    )
    print(f"== {old_root}: {len(candidates)} candidate {suffix} file(s)",
          file=sys.stderr)
    for src in candidates:
        dst = plan_move(src, new_root, suffix)
        if dst is None:
            skipped += 1
            continue
        if dst == src:
            # Would move file onto itself - unreachable in current layout
            # but defends against future config slips.
            print(f"  skip {src.name}: destination equals source",
                  file=sys.stderr)
            skipped += 1
            continue
        if dst.exists():
            print(f"  skip {src.name}: destination already exists ({dst})",
                  file=sys.stderr)
            skipped += 1
            continue
        print(f"  mv {src} -> {dst}", file=sys.stderr)
        if not dry_run:
            dst.parent.mkdir(parents=True, exist_ok=True)
            # Use shutil.move so we work across drives if old/new roots differ.
            shutil.move(str(src), str(dst))
        moved += 1
    return (moved, skipped)


def main() -> int:
    args = parse_args()

    l1_moved, l1_skipped = migrate_directory(
        Path(args.l1_old_root),
        Path(args.l1_new_root),
        ".mbp1.csv",
        args.dry_run,
    )
    l2_moved, l2_skipped = migrate_directory(
        Path(args.l2_old_root),
        Path(args.l2_new_root),
        ".mbp10.csv",
        args.dry_run,
    )

    verb = "would move" if args.dry_run else "moved"
    print(
        f"\nL1: {verb} {l1_moved} file(s), skipped {l1_skipped}\n"
        f"L2: {verb} {l2_moved} file(s), skipped {l2_skipped}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
