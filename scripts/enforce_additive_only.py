
#!/usr/bin/env python3
import argparse
import difflib
import json
from pathlib import Path

TEXT_EXTS = {".hpp",".h",".cpp",".cc",".cxx",".c",".ipp",".py",".md",".txt",".ini",".cmake",".yaml",".yml"}

def collect(root: Path):
    out = {}
    for p in root.rglob("*"):
        if p.is_file():
            out[p.relative_to(root).as_posix()] = p
    return out

def read_text(p: Path):
    try:
        return p.read_text(encoding="utf-8").splitlines()
    except Exception:
        return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True)
    ap.add_argument("--candidate", required=True)
    args = ap.parse_args()

    base = Path(args.base)
    cand = Path(args.candidate)
    b = collect(base)
    c = collect(cand)

    removed_files = sorted(set(b) - set(c))
    removed_lines = []

    for rel in sorted(set(b) & set(c)):
        if b[rel].suffix.lower() not in TEXT_EXTS:
            continue
        bt = read_text(b[rel])
        ct = read_text(c[rel])
        if bt is None or ct is None:
            continue
        for line in difflib.ndiff(bt, ct):
            if line.startswith("- "):
                removed_lines.append({"file": rel, "line": line[2:]})

    ok = not removed_files and not removed_lines
    print(json.dumps({
        "ok": ok,
        "removed_files": removed_files,
        "removed_line_count": len(removed_lines),
        "removed_lines_preview": removed_lines[:50],
    }, indent=2))
    raise SystemExit(0 if ok else 1)

if __name__ == "__main__":
    main()
