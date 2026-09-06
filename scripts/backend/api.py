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
        "has_qc": (d / "qc_result.json").is_file(),
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


# --------------------------------------------------------- config schema
#
# Drives the app's dynamic config form. Base knobs apply to every
# strategy; per-branch knobs are appended only when the selected
# binary's branch matches -- so the app shows "only configs available
# for that branch" (the user's requirement). A field descriptor is
# {key, label, type, default, section, [options], [help]}.

_BASE_CONFIG_SCHEMA: List[Dict[str, Any]] = [
    {"key": "run_label", "label": "Label", "type": "string", "default": "",
     "section": "run"},
    {"key": "databento_start", "label": "Start (UTC)", "type": "datetime",
     "default": "", "section": "window"},
    {"key": "databento_end", "label": "End (UTC)", "type": "datetime",
     "default": "", "section": "window"},
    {"key": "symbol_universe_path", "label": "Universe file", "type": "string",
     "default": "config/symbols_yen.txt", "section": "universe"},
    {"key": "universe_size", "label": "Universe size", "type": "int",
     "default": 49, "section": "universe"},
    {"key": "top_k", "label": "Top K", "type": "int", "default": 3,
     "section": "universe"},
    {"key": "target_profit_pct", "label": "Target profit %", "type": "float",
     "default": 0.008, "section": "strategy",
     "help": "Sell target and the minimum forecast to enter."},
    {"key": "trade_notional", "label": "Per-slot $", "type": "int",
     "default": 500, "section": "sizing"},
    {"key": "account_budget", "label": "Account budget $", "type": "int",
     "default": 1500, "section": "sizing"},
    {"key": "entry_limit_mode", "label": "Entry limit", "type": "enum",
     "default": "ask", "options": ["ask", "mid"], "section": "execution"},
    {"key": "order_enabled", "label": "Place orders", "type": "bool",
     "default": True, "section": "execution",
     "help": "Off = dry run (decisions logged, no orders placed)."},
    {"key": "commission_per_share", "label": "Commission/share", "type": "float",
     "default": 0.0035, "section": "costs"},
    {"key": "commission_min_per_order", "label": "Commission min/order",
     "type": "float", "default": 0.35, "section": "costs"},
    {"key": "half_spread_cost", "label": "Half-spread cost", "type": "float",
     "default": 0.0005, "section": "costs"},
    {"key": "impact_coefficient", "label": "Impact coeff", "type": "float",
     "default": 0.1, "section": "costs"},
]

# Extra knobs unlocked per branch. Keyed by the branch name recorded in
# a binary's manifest (bin/versions/<v>/binary.json {"branch": ...}).
_BRANCH_CONFIG_SCHEMA: Dict[str, List[Dict[str, Any]]] = {
    "chronos2-mr-pred-exit": [
        {"key": "strategy_mode", "label": "Strategy", "type": "const",
         "default": "chronos2_mr_pred_exit", "section": "strategy"},
        {"key": "chronos2_model", "label": "Chronos-2 model", "type": "string",
         "default": "amazon/chronos-2", "section": "chronos2"},
        {"key": "chronos2_context_len", "label": "Context len", "type": "int",
         "default": 64, "section": "chronos2"},
        {"key": "chronos2_prediction_len", "label": "Prediction len",
         "type": "int", "default": 1, "section": "chronos2"},
        {"key": "chronos2_max_annual_vol", "label": "Max annual vol",
         "type": "float", "default": 0.80, "section": "chronos2"},
        {"key": "chronos2_vol_floor", "label": "Vol floor", "type": "float",
         "default": 0.05, "section": "chronos2"},
        {"key": "chronos2_reinvest_increment", "label": "Reinvest step $",
         "type": "int", "default": 500, "section": "chronos2"},
    ],
}


def _config_schema_for_branch(branch: Optional[str]) -> List[Dict[str, Any]]:
    schema = list(_BASE_CONFIG_SCHEMA)
    if branch and branch in _BRANCH_CONFIG_SCHEMA:
        schema += _BRANCH_CONFIG_SCHEMA[branch]
    return schema


def _list_binaries() -> List[Dict[str, Any]]:
    """Enumerates runnable binaries: the default bin/hft_app plus every
    bin/versions/<version>/. Each carries an optional binary.json
    manifest {branch, description, built_at, commit}; when absent we
    report branch=None and the base config schema only.
    """
    out: List[Dict[str, Any]] = []
    bin_dir = REPO_ROOT / "bin"

    def _manifest(d: Path) -> Dict[str, Any]:
        mf = d / "binary.json"
        if mf.is_file():
            try:
                return json.loads(mf.read_text())
            except Exception:
                return {}
        return {}

    # The current default binary.
    default_bin = bin_dir / "hft_app"
    if default_bin.exists():
        mf = _manifest(bin_dir)
        branch = mf.get("branch")
        out.append({
            "version": "current",
            "is_default": True,
            "branch": branch,
            "description": mf.get("description", "default bin/hft_app"),
            "built_at": mf.get("built_at"),
            "commit": mf.get("commit"),
            "config_schema": _config_schema_for_branch(branch),
        })

    versions_dir = bin_dir / "versions"
    if versions_dir.is_dir():
        for d in sorted(versions_dir.iterdir(), reverse=True):
            if not d.is_dir() or not (d / "hft_app").exists():
                continue
            mf = _manifest(d)
            branch = mf.get("branch")
            out.append({
                "version": d.name,
                "is_default": False,
                "branch": branch,
                "description": mf.get("description", d.name),
                "built_at": mf.get("built_at"),
                "commit": mf.get("commit"),
                "config_schema": _config_schema_for_branch(branch),
            })
    return out


@app.get("/binaries")
def list_binaries(req: Request):
    """Runnable binaries + each one's branch and config schema, so the
    app can offer branch selection and show only the configs that branch
    supports."""
    _require_token(req)
    return {"binaries": _list_binaries()}


@app.get("/runs/{run_id}/qc")
def get_run_qc(run_id: str, req: Request):
    """Serves the QuantConnect-format result document for a run."""
    _require_token(req)
    if "/" in run_id or run_id.startswith(".."):
        raise HTTPException(status_code=400, detail="bad id")
    qc = RUNS_DIR / run_id / "qc_result.json"
    if not qc.is_file():
        raise HTTPException(status_code=404, detail="qc_result.json not found")
    try:
        return json.loads(qc.read_text())
    except Exception as exc:
        raise HTTPException(status_code=500, detail=f"parse error: {exc}")


# ------------------------------------------------------------ live launch
#
# Starts the engine against the IB Gateway (paper 4002 / live 4001) via
# the hft_app systemd unit. Guarded: paper needs confirm=true, live
# needs confirm=true AND confirm_live=true, and we refuse to start if the
# gateway socket for that mode isn't accepting connections.

def _gateway_reachable(port: int) -> bool:
    import socket
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=3):
            return True
    except OSError:
        return False


@app.post("/live/start")
def live_start(payload: Dict[str, Any], req: Request):
    """Start live/paper trading.

    Body:
      {
        "mode": "paper" | "live",
        "confirm": true,                  # required
        "confirm_live": true,             # required when mode == "live"
        "binary_version": "v14"           # optional; swaps bin/hft_app
      }
    Refuses if the IB Gateway socket for the mode isn't up, or if an
    hft_app is already running.
    """
    _require_token(req)
    mode = str(payload.get("mode", "paper")).lower()
    if mode not in ("paper", "live"):
        raise HTTPException(status_code=400, detail="mode must be paper|live")
    if not payload.get("confirm"):
        raise HTTPException(status_code=400, detail="confirm=true required")
    if mode == "live" and not payload.get("confirm_live"):
        raise HTTPException(
            status_code=400,
            detail="confirm_live=true required to start LIVE trading",
        )

    # Refuse to double-start.
    running = subprocess.run(
        ["pgrep", "-f", HFT_APP_PATTERN],
        capture_output=True, text=True, check=False,
    ).stdout.strip()
    if running:
        raise HTTPException(status_code=409, detail="hft_app already running")

    port = 4001 if mode == "live" else 4002
    if not _gateway_reachable(port):
        raise HTTPException(
            status_code=503,
            detail=f"IB Gateway not reachable on 127.0.0.1:{port} "
                   f"(mode={mode}); is hft_ibgateway up and logged in?",
        )

    # Optional binary swap: point bin/hft_app at the chosen version.
    version = payload.get("binary_version")
    if version and version != "current":
        target = REPO_ROOT / "bin" / "versions" / version / "hft_app"
        if not target.exists():
            raise HTTPException(status_code=404,
                                detail=f"binary_version {version} not found")
        link = REPO_ROOT / "bin" / "hft_app"
        try:
            if link.is_symlink() or link.exists():
                link.unlink()
            link.symlink_to(target)
        except Exception as exc:
            raise HTTPException(status_code=500,
                                detail=f"binary swap failed: {exc}")

    # Point config.ini at the right IBKR mode, then start the unit.
    _set_broker_mode(mode)
    try:
        subprocess.run(["systemctl", "start", "hft_app"],
                       capture_output=True, text=True, check=True)
    except subprocess.CalledProcessError as exc:
        raise HTTPException(status_code=500,
                            detail=f"systemctl start failed: {exc.stderr}")
    return {"started": True, "mode": mode, "port": port,
            "binary_version": version or "current"}


@app.post("/live/stop")
def live_stop(req: Request):
    """Stop the engine (systemctl stop hft_app). For an emergency
    freeze/flatten while keeping the process up, use /kill or /liquidate
    instead."""
    _require_token(req)
    try:
        subprocess.run(["systemctl", "stop", "hft_app"],
                       capture_output=True, text=True, check=True)
    except subprocess.CalledProcessError as exc:
        raise HTTPException(status_code=500,
                            detail=f"systemctl stop failed: {exc.stderr}")
    return {"stopped": True}


def _set_broker_mode(mode: str) -> None:
    """Rewrites the [broker] mode + paper/live port in config.ini so the
    engine connects to the IB Gateway for the requested mode. Minimal
    line edit -- leaves every other config line untouched."""
    cfg_path = REPO_ROOT / "config.ini"
    if not cfg_path.is_file():
        return
    lines = cfg_path.read_text().splitlines()
    out = []
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("mode=") and (
            "backtest" in stripped or "paper" in stripped or "live" in stripped
            or "sim" in stripped
        ):
            out.append("mode=ibkr_paper" if mode == "paper" else "mode=live")
        elif stripped.startswith("paper_port="):
            out.append("paper_port=4002")
        elif stripped.startswith("live_port="):
            out.append("live_port=4001")
        else:
            out.append(line)
    cfg_path.write_text("\n".join(out) + "\n")


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
