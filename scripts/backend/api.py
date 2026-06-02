#!/usr/bin/env python3
"""HFT backend API.

FastAPI service running on Hetzner that the mobile app (or any HTTP
client) reads to monitor live trading + list backtests + launch new
backtests. Exposed only over wireguard / SSH tunnel -- this is not
hardened for the public internet.

Endpoints implemented today:
  GET  /health                         liveness + version
  GET  /runs                           list of run folders + headline metrics
  GET  /runs/{id}                      full per-run detail (metrics.json
                                        + orders + decisions head)
  GET  /live/status                    hft_app process state + RSS + last log
  GET  /databento/credits              remaining Databento balance
  POST /backtests                      launch a new backtest with overrides

Endpoints PLANNED (stubbed):
  GET  /backtests                      live/queued backtest runs
  GET  /backtests/{id}                 detail of a specific running backtest
  POST /chat                           proxy to Claude / OpenAI for incident
                                        investigation

Auth: bearer token from `X-HFT-Token` header matched against
`/etc/hft/api.env`'s `API_TOKEN`. Mobile app embeds the token in
keychain.

Run:
  pip install fastapi uvicorn
  uvicorn scripts.backend.api:app --host 127.0.0.1 --port 8088

systemd unit lives in `scripts/systemd/hft_backend.service` -- not
yet written; future ops work.
"""

from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path
from typing import Any, Dict, List, Optional

try:
    from fastapi import FastAPI, HTTPException, Header, Request
    from fastapi.responses import JSONResponse
except ImportError as exc:  # pragma: no cover - install hint
    raise SystemExit(
        "fastapi is required: pip install fastapi uvicorn"
    ) from exc


REPO_ROOT = Path(
    os.environ.get("HFT_REPO", "/mnt/HC_Volume_105581071/trading-system")
)
RUNS_DIR = REPO_ROOT / "reports" / "runs"
LOGS_DIR = REPO_ROOT / "logs"
HFT_APP_PATTERN = "bin/hft_app"
QUEUE_DIR = REPO_ROOT / "queue"
LAUNCHER_STATE_FILE = Path("/var/run/hft_backtest_launcher.state")


app = FastAPI(title="HFT Backend", version="0.1.0")


# ---------------------------------------------------------------------- auth


def _require_token(req: Request) -> None:
    """Pulls API_TOKEN from /etc/hft/api.env and compares against the
    request's X-HFT-Token header. Set API_TOKEN= (empty) to disable
    auth -- only for local dev.
    """
    expected = ""
    env_file = Path("/etc/hft/api.env")
    if env_file.is_file():
        for line in env_file.read_text().splitlines():
            t = line.strip()
            if t.startswith("API_TOKEN="):
                expected = t.split("=", 1)[1].strip().strip('"').strip("'")
                break
    if not expected:
        return  # auth disabled
    got = req.headers.get("x-hft-token", "")
    if got != expected:
        raise HTTPException(status_code=401, detail="bad token")


# ---------------------------------------------------------------------- helpers


def _read_metrics(run_dir: Path) -> Dict[str, Any]:
    m = run_dir / "metrics.json"
    if not m.is_file():
        return {}
    try:
        return json.loads(m.read_text())
    except Exception:
        return {}


def _hft_app_status() -> Dict[str, Any]:
    """Returns running / pid / rss_mb / last log lines."""
    out = {"running": False, "pid": None, "rss_mb": None, "elapsed": None}
    try:
        pgrep = subprocess.run(
            ["pgrep", "-fao", HFT_APP_PATTERN],
            capture_output=True, text=True, check=False,
        )
        if pgrep.returncode == 0 and pgrep.stdout.strip():
            out["running"] = True
            pid = int(pgrep.stdout.split()[0])
            out["pid"] = pid
            ps = subprocess.run(
                ["ps", "-p", str(pid), "-o", "rss=,etime="],
                capture_output=True, text=True, check=False,
            )
            if ps.returncode == 0:
                parts = ps.stdout.split()
                if len(parts) >= 2:
                    out["rss_mb"] = int(parts[0]) // 1024
                    out["elapsed"] = parts[1]
    except FileNotFoundError:
        pass
    return out


def _last_log_lines(n: int = 20) -> List[str]:
    log = LOGS_DIR / "hft_app.log"
    if not log.is_file():
        return []
    try:
        # tail; deque keeps memory bounded for huge logs.
        from collections import deque
        with log.open("rb") as f:
            tail = deque(f, maxlen=n)
        return [b.decode("utf-8", errors="replace").rstrip()
                for b in tail]
    except Exception:
        return []


# ---------------------------------------------------------------------- routes


@app.get("/health")
def health():
    return {"ok": True, "version": app.version}


@app.get("/runs")
def list_runs(req: Request):
    _require_token(req)
    if not RUNS_DIR.is_dir():
        return {"runs": []}
    rows = []
    for d in sorted(RUNS_DIR.iterdir(), key=lambda p: p.name, reverse=True):
        if not d.is_dir():
            continue
        m = _read_metrics(d)
        rows.append({
            "id": d.name,
            "n_round_trips_closed": m.get("n_round_trips_closed"),
            "realized_pnl_net": m.get("realized_pnl_net"),
            "win_rate": m.get("win_rate"),
            "sharpe_ratio_annualized": m.get("sharpe_ratio_annualized"),
            "avg_holding_minutes": m.get("avg_holding_minutes"),
            "has_metrics": bool(m),
        })
    return {"runs": rows}


@app.get("/runs/{run_id}")
def get_run(run_id: str, req: Request):
    _require_token(req)
    # Defensive: no path traversal.
    if "/" in run_id or run_id.startswith(".."):
        raise HTTPException(status_code=400, detail="bad id")
    d = RUNS_DIR / run_id
    if not d.is_dir():
        raise HTTPException(status_code=404, detail="run not found")
    metrics = _read_metrics(d)
    orders_head: List[str] = []
    orders_file = d / "orders.csv"
    if orders_file.is_file():
        try:
            with orders_file.open() as f:
                orders_head = [next(f).rstrip() for _ in range(50)]
        except StopIteration:
            pass
        except Exception:
            pass
    return {
        "id": run_id,
        "metrics": metrics,
        "orders_head": orders_head,
        "has_report_html": (d / "report.html").is_file(),
    }


@app.get("/live/status")
def live_status(req: Request):
    _require_token(req)
    return {
        "process": _hft_app_status(),
        "log_tail": _last_log_lines(30),
    }


@app.get("/databento/credits")
def databento_credits(req: Request):
    """Returns remaining Databento balance + cost-to-date.

    Wraps the existing `scripts/databento_l1_cost_quote.py` machinery
    -- specifically `databento.metadata.get_balance()`. The actual
    balance API path depends on the user's plan; if get_balance
    isn't available we fall back to "manual check at databento.com".
    """
    _require_token(req)
    try:
        import databento as db  # noqa: F401
    except ImportError:
        return {
            "available": False,
            "reason": "databento python package not installed",
            "manual_url": "https://databento.com/account/billing",
        }
    try:
        # The exact API name may vary by databento client version.
        # We try the common names; first one that works wins.
        client = db.Historical(
            key=os.environ.get("DATABENTO_API_KEY", ""),
        )
        for attr in ("get_balance", "balance", "credit_balance"):
            f = getattr(client.metadata, attr, None)
            if callable(f):
                v = f()
                return {"available": True, "raw": v}
        return {
            "available": False,
            "reason": "no balance endpoint on this client version",
            "manual_url": "https://databento.com/account/billing",
        }
    except Exception as exc:
        return {
            "available": False,
            "reason": f"databento API call failed: {exc}",
            "manual_url": "https://databento.com/account/billing",
        }


@app.post("/backtests")
def launch_backtest(payload: Dict[str, Any], req: Request):
    """Enqueue a new backtest. The launcher daemon
    (scripts/hft_backtest_launcher.py) picks the job up from
    queue/incoming/ and runs it.

    Body:
      {
        "config": "config.databento_backtest.yen.ini",
        "label":  "yen_v5",
        "target_profit_pct": 0.008,
        "start":  "2024-08-02T13:30:00Z",
        "end":    "2024-08-09T20:00:00Z",
        "symbols": "config/symbols_yen.txt",
        "binary_version": "v14"        # optional
      }
    All keys optional except `config`.

    Returns the job id assigned. The job moves through
      queue/incoming -> queue/running -> queue/done
    as the launcher processes it. Poll GET /backtests for state.
    """
    _require_token(req)
    cfg = payload.get("config")
    if not cfg:
        raise HTTPException(status_code=400, detail="config is required")
    # id = label + a wall-clock suffix so two requests with the same
    # label don't collide on disk.
    import time as _t
    label = payload.get("label", "unnamed")
    job_id = f"{label}-{int(_t.time())}"
    job = dict(payload)
    job["id"] = job_id
    job["enqueued_at"] = _t.strftime("%Y-%m-%dT%H:%M:%SZ", _t.gmtime())
    incoming = QUEUE_DIR / "incoming"
    incoming.mkdir(parents=True, exist_ok=True)
    job_path = incoming / f"{job_id}.job.json"
    job_path.write_text(json.dumps(job, indent=2))
    return {
        "id": job_id,
        "queued_at": job["enqueued_at"],
        "queue_path": str(job_path),
    }


@app.get("/backtests")
def list_backtests(req: Request):
    """List queued / running / recently-done backtests by reading the
    launcher's state file + queue directories.
    """
    _require_token(req)
    queued: List[str] = []
    running: List[str] = []
    done: List[str] = []
    inc = QUEUE_DIR / "incoming"
    run = QUEUE_DIR / "running"
    dn = QUEUE_DIR / "done"
    if inc.is_dir():
        queued = sorted(p.name for p in inc.glob("*.job.json"))
    if run.is_dir():
        running = sorted(p.name for p in run.glob("*.job.json"))
    if dn.is_dir():
        # Newest 25 done.
        done = sorted(
            (p.name for p in dn.glob("*.job.json")),
            reverse=True,
        )[:25]
    launcher_state: Dict[str, Any] = {}
    if LAUNCHER_STATE_FILE.exists():
        try:
            launcher_state = json.loads(LAUNCHER_STATE_FILE.read_text())
        except Exception:
            pass
    return {
        "queued": queued,
        "running": running,
        "done": done,
        "launcher": launcher_state,
    }


@app.get("/backtests/{job_id}")
def backtest_detail(job_id: str, req: Request):
    """Detail of a single job. Returns the job spec + result (when
    available) + a path to the per-run report folder if archived."""
    _require_token(req)
    if "/" in job_id or job_id.startswith(".."):
        raise HTTPException(status_code=400, detail="bad id")
    # Find the job in any of the three buckets.
    for bucket in ("running", "incoming", "done"):
        p = QUEUE_DIR / bucket / f"{job_id}.job.json"
        if p.is_file():
            try:
                spec = json.loads(p.read_text())
            except Exception:
                spec = {}
            out: Dict[str, Any] = {"id": job_id, "bucket": bucket, "spec": spec}
            res = QUEUE_DIR / "done" / f"{job_id}.result.json"
            if res.is_file():
                try:
                    out["result"] = json.loads(res.read_text())
                except Exception:
                    pass
            return out
    raise HTTPException(status_code=404, detail="job not found")


@app.post("/kill")
def kill_signal(req: Request):
    """Delivers SIGUSR1 to every hft_app process. The engine treats it
    as "freeze trader": cancel every open entry+exit, refuse new orders,
    keep open positions in place. Idempotent (already-frozen sessions
    just log the second signal).
    """
    _require_token(req)
    return _send_signal_to_hft_app("USR1")


@app.post("/liquidate")
def liquidate_signal(req: Request):
    """Delivers SIGUSR2 to every hft_app process. The engine treats it
    as "force liquidate": freeze trader + post marketable sells at
    best_bid for every open position. Use when something is wrong
    enough that holding is riskier than the immediate exit prints.
    """
    _require_token(req)
    return _send_signal_to_hft_app("USR2")


def _send_signal_to_hft_app(signal: str) -> Dict[str, Any]:
    """Common implementation for /kill and /liquidate. Looks up the
    pid via pgrep so we don't depend on systemctl returning the right
    thing for a process that systemd may not own (manual launch).
    """
    try:
        out = subprocess.run(
            ["pgrep", "-f", HFT_APP_PATTERN],
            capture_output=True, text=True, check=False,
        )
        pids = [p for p in out.stdout.strip().splitlines() if p]
        if not pids:
            return {"sent_to": [], "reason": "no hft_app running"}
        # `kill -USR1 1234 5678` -- works on bash and POSIX kill.
        subprocess.run(
            ["kill", f"-{signal}", *pids],
            capture_output=True, text=True, check=False,
        )
        return {"sent_to": pids, "signal": signal}
    except FileNotFoundError:
        return {"sent_to": [], "reason": "pgrep / kill unavailable"}


@app.post("/chat")
def chat(payload: Dict[str, Any], req: Request):
    """Proxy to Claude / OpenAI / Cursor for incident investigation.

    Body:
      {
        "platform": "claude" | "openai" | "cursor",
        "message":  "free-form description of the issue",
        "include_log_tail": true
      }
    """
    _require_token(req)
    # TODO: wire the Anthropic / OpenAI / Cursor APIs. Out of scope
    # for the first backend rev -- the mobile app gets the endpoint
    # contract right and we fill in the actual API call later.
    return JSONResponse(
        status_code=501,
        content={"detail": "chat backend not implemented yet",
                 "platforms_planned": ["claude", "openai", "cursor"]},
    )
