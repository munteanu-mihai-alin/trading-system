#!/usr/bin/env python3
"""Backtest launcher daemon.

Backlog item 8. Long-running process that decouples the FastAPI
backend from actual backtest execution and adds two responsibilities
the API by itself can't handle:

  1. **Queue**: serialises backtest launches so we don't accidentally
     start two heavy runs in parallel and OOM the Hetzner box. The
     queue is a directory; the API drops a `*.job.json` file and the
     launcher picks it up.

  2. **Binary versioning**: watches `bin/incoming/` for fresh CI build
     artifacts (tarballs from the CI workflow), extracts them into
     `bin/versions/<id>/`, and lets each job pin a specific binary
     version. The default symlink `bin/hft_app -> bin/versions/<latest>`
     gets advanced atomically.

The hft_monitor daemon (scripts/hft_monitor.py) watches this process
as part of the umbrella "monitor everything that matters". The
launcher writes liveness + last-job state to
/var/run/hft_backtest_launcher.state so the monitor (and the
backend's GET /backtests) can poll it cheaply.

Queue layout under $HFT_REPO/queue/:

  queue/incoming/<id>.job.json    written by the backend
  queue/running/<id>.job.json     moved here when the launcher starts it
  queue/done/<id>.job.json        moved here on completion;
                                  accompanied by <id>.result.json
  queue/state.json                aggregate state (current job, history)

Job file shape (mirrors the backend's POST /backtests body):

  {
    "id":               "yen-v5-2026-06-01T13:25",
    "config":           "config.databento_backtest.yen.ini",
    "label":            "yen_v5",
    "target_profit_pct": 0.008,
    "start":            "2024-08-02T13:30:00Z",
    "end":              "2024-08-09T20:00:00Z",
    "symbols":          "config/symbols_yen.txt",
    "binary_version":   "v14"         // optional; default = current bin/hft_app
  }

Usage:
  scripts/hft_backtest_launcher.py [--once] [--repo $HFT_REPO]

Designed to run as a systemd service
(scripts/systemd/hft_backtest_launcher.service); --once is for manual
poking during dev.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import tarfile
import time
from pathlib import Path
from typing import Any, Dict, List, Optional


DEFAULT_POLL_SEC = 5.0
STATE_FILE = Path("/var/run/hft_backtest_launcher.state")


def _now_iso() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def write_state(state: Dict[str, Any]) -> None:
    """Atomic write so a torn read by hft_monitor / the backend gets
    either the old or the new value, never a partial JSON."""
    try:
        STATE_FILE.parent.mkdir(parents=True, exist_ok=True)
        tmp = STATE_FILE.with_suffix(".tmp")
        tmp.write_text(json.dumps(state, indent=2))
        tmp.replace(STATE_FILE)
    except Exception as exc:
        print(f"launcher: state write failed: {exc}", file=sys.stderr)


def load_state() -> Dict[str, Any]:
    try:
        return json.loads(STATE_FILE.read_text())
    except Exception:
        return {"history": [], "current": None, "started_at": _now_iso()}


# --------------------------------------------------------------- binaries


def scan_incoming_binaries(incoming_dir: Path, versions_dir: Path) -> int:
    """Move new tarballs from `bin/incoming/` into versioned dirs.

    Each `*.tar.gz` is extracted into `bin/versions/<basename>/` where
    `<basename>` is the tarball's stem (e.g. `hft_app.v14.tar.gz` ->
    `bin/versions/hft_app.v14/`). On success the tarball is moved to
    `bin/incoming/_applied/` so a future scan doesn't redo work.

    Returns the count of binaries newly applied.
    """
    if not incoming_dir.is_dir():
        return 0
    applied_dir = incoming_dir / "_applied"
    applied_dir.mkdir(exist_ok=True)
    versions_dir.mkdir(parents=True, exist_ok=True)
    applied = 0
    for tar_path in sorted(incoming_dir.glob("*.tar.gz")):
        version_id = tar_path.stem  # strips one suffix; "x.tar.gz" -> "x.tar"
        if version_id.endswith(".tar"):
            version_id = version_id[: -len(".tar")]
        target = versions_dir / version_id
        if target.exists():
            # Already extracted; just archive the duplicate tarball.
            tar_path.replace(applied_dir / tar_path.name)
            continue
        try:
            target.mkdir(parents=True, exist_ok=True)
            with tarfile.open(tar_path, "r:gz") as t:
                t.extractall(target)  # noqa: S202 - controlled input
            # Make hft_app executable; CI tarballs don't always preserve it.
            for binary in target.rglob("hft_app"):
                binary.chmod(0o755)
            tar_path.replace(applied_dir / tar_path.name)
            applied += 1
            print(f"launcher: applied binary {version_id}", file=sys.stderr)
        except Exception as exc:
            print(f"launcher: failed to extract {tar_path}: {exc}",
                  file=sys.stderr)
    return applied


def resolve_binary_for_job(
    repo: Path, binary_version: Optional[str]
) -> Optional[Path]:
    """Resolves the hft_app path for a given binary_version pin.

    Returns the path or None when the version doesn't exist. The
    default (binary_version=None) is whatever bin/hft_app currently
    points to.
    """
    if not binary_version:
        candidate = repo / "bin" / "hft_app"
        return candidate if candidate.exists() else None
    candidate = repo / "bin" / "versions" / binary_version / "hft_app"
    if candidate.exists():
        return candidate
    return None


# --------------------------------------------------------------- jobs


def run_job(job: Dict[str, Any], repo: Path) -> Dict[str, Any]:
    """Spawns scripts/hft_backtest.sh for this job. Blocks until done.

    Returns a result dict (returncode, stdout/stderr tails, wall_secs).
    Errors are caught and surfaced; the launcher never crashes on a
    bad job -- it records the failure and moves on to the next one.
    """
    started = time.time()
    args = [str(repo / "scripts" / "hft_backtest.sh")]
    for key, flag in (
        ("config", "--config"),
        ("label", "--label"),
        ("target_profit_pct", "--target"),
        ("start", "--start"),
        ("end", "--end"),
        ("symbols", "--symbols"),
    ):
        v = job.get(key)
        if v is not None:
            args += [flag, str(v)]
    # Binary override: if the job pins a version, point the script at
    # it via env var. scripts/hft_backtest.sh reads $HFT_BIN if set.
    env = dict(os.environ)
    bin_path = resolve_binary_for_job(repo, job.get("binary_version"))
    if bin_path is not None:
        env["HFT_BIN"] = str(bin_path)
    try:
        out = subprocess.run(
            args,
            cwd=str(repo),
            env=env,
            capture_output=True,
            text=True,
            timeout=job.get("timeout_sec", 6 * 3600),
            check=False,
        )
        result = {
            "started_at": started,
            "finished_at": time.time(),
            "wall_secs": time.time() - started,
            "returncode": out.returncode,
            "stdout_tail": "\n".join(out.stdout.splitlines()[-40:]),
            "stderr_tail": "\n".join(out.stderr.splitlines()[-40:]),
            "ok": out.returncode == 0,
        }
    except subprocess.TimeoutExpired as exc:
        result = {
            "started_at": started,
            "finished_at": time.time(),
            "wall_secs": time.time() - started,
            "returncode": -1,
            "stdout_tail": exc.stdout or "",
            "stderr_tail": "TIMEOUT",
            "ok": False,
        }
    except Exception as exc:
        result = {
            "started_at": started,
            "finished_at": time.time(),
            "wall_secs": time.time() - started,
            "returncode": -2,
            "stdout_tail": "",
            "stderr_tail": f"launcher exception: {exc}",
            "ok": False,
        }
    return result


def take_next_job(incoming: Path, running: Path) -> Optional[Path]:
    """Picks the OLDEST *.job.json in incoming/, moves to running/.

    Atomic via rename so two launcher instances (we hope there's only
    one, but defensive) can't grab the same job.
    """
    candidates = sorted(incoming.glob("*.job.json"),
                        key=lambda p: p.stat().st_mtime)
    for c in candidates:
        target = running / c.name
        try:
            c.rename(target)
            return target
        except (FileNotFoundError, OSError):
            # Another launcher (or the dev) grabbed it; try the next.
            continue
    return None


def finish_job(running_path: Path, done_dir: Path, result: Dict[str, Any]) -> None:
    """Moves running/<id>.job.json -> done/<id>.job.json and writes
    done/<id>.result.json next to it.
    """
    done_dir.mkdir(parents=True, exist_ok=True)
    job_target = done_dir / running_path.name
    try:
        running_path.rename(job_target)
    except Exception as exc:
        print(f"launcher: rename to done failed: {exc}", file=sys.stderr)
    try:
        result_path = job_target.with_suffix("").with_suffix(".result.json")
        result_path.write_text(json.dumps(result, indent=2))
    except Exception as exc:
        print(f"launcher: result write failed: {exc}", file=sys.stderr)


# --------------------------------------------------------------- driver


def main(argv: List[str]) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--once", action="store_true",
                   help="Process one job (or nothing) then exit. Dev only.")
    p.add_argument("--repo", type=Path,
                   default=Path(os.environ.get(
                       "HFT_REPO",
                       "/mnt/HC_Volume_105581071/trading-system")))
    p.add_argument("--poll-sec", type=float, default=DEFAULT_POLL_SEC)
    args = p.parse_args(argv)

    repo: Path = args.repo
    queue_dir = repo / "queue"
    incoming = queue_dir / "incoming"
    running = queue_dir / "running"
    done = queue_dir / "done"
    bin_incoming = repo / "bin" / "incoming"
    bin_versions = repo / "bin" / "versions"
    for d in (incoming, running, done, bin_incoming, bin_versions):
        d.mkdir(parents=True, exist_ok=True)

    state = load_state()
    state["started_at"] = state.get("started_at") or _now_iso()
    state["repo"] = str(repo)
    write_state(state)

    stop = {"flag": False}

    def handle_term(signum, frame):
        stop["flag"] = True
        print("launcher: stop requested, finishing current loop",
              file=sys.stderr)

    signal.signal(signal.SIGTERM, handle_term)
    signal.signal(signal.SIGINT, handle_term)

    # On startup, recover anything left in `running/` from a prior
    # process death. Either re-queue or mark as failed depending on
    # policy; safest is mark-as-failed because we don't know how far
    # the prior run got and re-running could double-spend Databento.
    for stuck in running.glob("*.job.json"):
        finish_job(stuck, done, {
            "ok": False,
            "stderr_tail": "abandoned by prior launcher process",
            "wall_secs": 0,
            "returncode": -3,
        })

    while not stop["flag"]:
        try:
            scan_incoming_binaries(bin_incoming, bin_versions)
            next_path = take_next_job(incoming, running)
            if next_path is None:
                state["current"] = None
                write_state(state)
                if args.once:
                    break
                # Cheap sleep so SIGTERM is responsive.
                slept = 0.0
                while not stop["flag"] and slept < args.poll_sec:
                    time.sleep(0.5)
                    slept += 0.5
                continue
            try:
                job = json.loads(next_path.read_text())
            except Exception as exc:
                finish_job(next_path, done, {
                    "ok": False,
                    "stderr_tail": f"job parse error: {exc}",
                    "wall_secs": 0,
                    "returncode": -4,
                })
                continue
            state["current"] = {
                "id": job.get("id", next_path.stem),
                "started_at": _now_iso(),
                "label": job.get("label"),
            }
            write_state(state)
            result = run_job(job, repo)
            finish_job(next_path, done, result)
            history = state.setdefault("history", [])
            history.append({
                "id": job.get("id", next_path.stem),
                "label": job.get("label"),
                "ok": result["ok"],
                "wall_secs": result["wall_secs"],
                "finished_at": _now_iso(),
            })
            # Cap history so the state file stays bounded.
            if len(history) > 200:
                state["history"] = history[-200:]
            state["current"] = None
            write_state(state)
        except Exception as exc:
            print(f"launcher: tick error: {exc}", file=sys.stderr)
            if args.once:
                return 1
        if args.once:
            break
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
