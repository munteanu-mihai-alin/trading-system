#!/usr/bin/env python3
import argparse
import re
from pathlib import Path

def parse_lcov(path: Path):
    lines_found = 0
    lines_hit = 0
    funcs_found = 0
    funcs_hit = 0
    branches_found = 0
    branches_hit = 0

    for raw in path.read_text(encoding="utf-8", errors="ignore").splitlines():
        if raw.startswith("LF:"):
            lines_found += int(raw.split(":")[1])
        elif raw.startswith("LH:"):
            lines_hit += int(raw.split(":")[1])
        elif raw.startswith("FNF:"):
            funcs_found += int(raw.split(":")[1])
        elif raw.startswith("FNH:"):
            funcs_hit += int(raw.split(":")[1])
        elif raw.startswith("BRF:"):
            branches_found += int(raw.split(":")[1])
        elif raw.startswith("BRH:"):
            branches_hit += int(raw.split(":")[1])

    def pct(hit, found):
        return 0.0 if found == 0 else 100.0 * hit / found

    return {
        "lines_found": lines_found,
        "lines_hit": lines_hit,
        "line_pct": pct(lines_hit, lines_found),
        "funcs_found": funcs_found,
        "funcs_hit": funcs_hit,
        "func_pct": pct(funcs_hit, funcs_found),
        "branches_found": branches_found,
        "branches_hit": branches_hit,
        "branch_pct": pct(branches_hit, branches_found),
    }

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--info", required=True)
    ap.add_argument("--threshold", type=float, default=70.0)
    args = ap.parse_args()

    stats = parse_lcov(Path(args.info))
    print(
        "Coverage summary: "
        f"lines={stats['line_pct']:.2f}% "
        f"functions={stats['func_pct']:.2f}% "
        f"branches={stats['branch_pct']:.2f}%"
    )

    if stats["line_pct"] < args.threshold:
        raise SystemExit(
            f"Line coverage {stats['line_pct']:.2f}% is below threshold {args.threshold:.2f}%"
        )

if __name__ == "__main__":
    main()
