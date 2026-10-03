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
import shutil
import subprocess
import time
import sys
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

INSTANCES_ROOT = Path(
    os.environ.get(
        "HFT_INSTANCES_ROOT", "/mnt/HC_Volume_105581071/trading-live"
    )
)
# Backtests are not a property of the paper/live instance the backend
# happens to serve -- they belong to the backtest instance, which owns
# the Databento caches and the run reports.
#
# This was wrong after the relocation and silently so: hft_backend runs
# with HFT_REPO=<paper>, so POST /backtests wrote jobs into
# paper/queue/incoming while hft_backtest_launcher, started with
# HFT_REPO=<backtest>, watched backtest/queue. Two submitted jobs sat
# in paper/queue for hours with the launcher active and idle, and
# nothing reported an error because neither side was broken -- they
# were just looking at different directories.
BACKTEST_DIR = Path(
    os.environ.get("HFT_BACKTEST_REPO", str(INSTANCES_ROOT / "backtest"))
)

RUNS_DIR = BACKTEST_DIR / "reports" / "runs"
LOGS_DIR = REPO_ROOT / "logs"
# ---- Instances -------------------------------------------------------
#
# paper and live are separate DIRECTORIES under the trading-live root,
# each with its own pinned binary, its own config.ini and its own
# systemd template instance. Mode is a property of the instance and is
# never rewritten at runtime -- that is the whole point of the layout.
#
# This replaces _set_broker_mode, which edited config.ini in place to
# flip paper/live. After the relocation that function had become
# actively dangerous: HFT_REPO points at the PAPER instance, so
# starting "live" from the app would have rewritten paper/config.ini to
# mode=live and then started hft_app@paper against port 4001 -- live
# trading out of the paper directory, with the paper config corrupted
# and the live directory untouched.
VALID_INSTANCES = ("paper", "live")
DEFAULT_INSTANCE = os.environ.get("HFT_INSTANCE", "paper")


def _resolve_instance(name: Optional[str]) -> str:
    """Validate an instance name, falling back to the default.

    Whitelisted rather than sanitised: the value reaches a filesystem
    path and a systemd unit name, and neither is a good place to find
    out a caller was creative.
    """
    inst = (name or DEFAULT_INSTANCE).strip().lower()
    if inst not in VALID_INSTANCES:
        raise HTTPException(
            status_code=400,
            detail=f"instance must be one of {list(VALID_INSTANCES)}",
        )
    return inst


def _instance_dir(inst: str) -> Path:
    return INSTANCES_ROOT / inst


def _instance_unit(inst: str) -> str:
    return f"hft_app@{inst}.service"


def _instance_pattern(inst: str) -> str:
    """pgrep pattern that matches ONLY this instance's engine.

    The bare "bin/hft_app" matched any instance -- and any command line
    that merely mentioned the path.
    """
    return f"trading-live/{inst}/bin/hft_app"


def _instance_mode(inst: str) -> Optional[str]:
    """The broker mode this instance is configured for. Read-only."""
    cfg = _instance_dir(inst) / "config.ini"
    try:
        for line in cfg.read_text().splitlines():
            stripped = line.strip()
            if stripped.startswith("mode="):
                return stripped.split("=", 1)[1].strip()
    except OSError:
        pass
    return None


HFT_APP_PATTERN = "bin/hft_app"  # legacy: any instance
QUEUE_DIR = BACKTEST_DIR / "queue"
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


def _host_resources() -> Dict[str, Any]:
    """Host memory and volume headroom, so the app can show the engine's
    RSS against something rather than as a bare number.

    /proc/meminfo and shutil rather than psutil: hft_monitor.py already
    reads meminfo the same way and deliberately avoids the dependency,
    and this runs on every status poll.

    MemAvailable, not MemFree. Free excludes reclaimable page cache, so
    on this box it reads a few hundred MB out of 7.6 GB and looks
    alarming while ~6.5 GB is genuinely available.
    """
    out: Dict[str, Any] = {
        "mem_total_mb": None,
        "mem_available_mb": None,
        "mem_used_mb": None,
        "disk_free_gb": None,
        "disk_total_gb": None,
    }
    try:
        fields = {}
        with open("/proc/meminfo") as f:
            for line in f:
                key, _, rest = line.partition(":")
                parts = rest.split()
                if parts:
                    fields[key] = int(parts[0])  # kB
        total = fields.get("MemTotal")
        avail = fields.get("MemAvailable")
        if total:
            out["mem_total_mb"] = total // 1024
        if avail:
            out["mem_available_mb"] = avail // 1024
        if total and avail:
            out["mem_used_mb"] = (total - avail) // 1024
    except (OSError, ValueError):
        pass

    try:
        usage = shutil.disk_usage(str(REPO_ROOT))
        out["disk_free_gb"] = round(usage.free / (1024 ** 3), 1)
        out["disk_total_gb"] = round(usage.total / (1024 ** 3), 1)
    except OSError:
        pass
    return out


def _hft_app_status(pattern: str = HFT_APP_PATTERN) -> Dict[str, Any]:
    """Returns running / pid / rss_mb / last log lines."""
    out = {"running": False, "pid": None, "rss_mb": None, "elapsed": None}
    try:
        pgrep = subprocess.run(
            ["pgrep", "-fao", pattern],
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


def _last_log_lines(n: int = 20,
                    log_dir: Optional[Path] = None) -> List[str]:
    log = (log_dir or LOGS_DIR) / "hft_app.log"
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
            import itertools
            with orders_file.open() as f:
                # islice never raises StopIteration mid-comprehension the
                # way `next()` in a range loop does -- that bug left
                # orders_head empty for any run with fewer than 50 lines.
                orders_head = [ln.rstrip() for ln in itertools.islice(f, 50)]
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
def live_status(req: Request, instance: Optional[str] = None):
    """Status of one instance. ?instance=paper|live, default paper."""
    _require_token(req)
    inst = _resolve_instance(instance)
    return {
        "instance": inst,
        "mode": _instance_mode(inst),
        "unit": _instance_unit(inst),
        "instances": list(VALID_INSTANCES),
        "process": _hft_app_status(_instance_pattern(inst)),
        "host": _host_resources(),
        "log_tail": _last_log_lines(30, _instance_dir(inst) / "logs"),
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
def kill_signal(req: Request, instance: Optional[str] = None):
    """Delivers SIGUSR1 to every hft_app process. The engine treats it
    as "freeze trader": cancel every open entry+exit, refuse new orders,
    keep open positions in place. Idempotent (already-frozen sessions
    just log the second signal).
    """
    _require_token(req)
    inst = _resolve_instance(instance)
    return _send_signal_to_hft_app("USR1", _instance_pattern(inst))


@app.post("/liquidate")
def liquidate_signal(req: Request, instance: Optional[str] = None):
    """Delivers SIGUSR2 to every hft_app process. The engine treats it
    as "force liquidate": freeze trader + post marketable sells at
    best_bid for every open position. Use when something is wrong
    enough that holding is riskier than the immediate exit prints.
    """
    _require_token(req)
    inst = _resolve_instance(instance)
    return _send_signal_to_hft_app("USR2", _instance_pattern(inst))


def _send_signal_to_hft_app(signal: str,
                            pattern: str = HFT_APP_PATTERN) -> Dict[str, Any]:
    """Common implementation for /kill and /liquidate. Looks up the
    pid via pgrep so we don't depend on systemctl returning the right
    thing for a process that systemd may not own (manual launch).
    """
    try:
        out = subprocess.run(
            ["pgrep", "-f", pattern],
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


def _probe_binary_provenance(exe: Path) -> Dict[str, Any]:
    """Ask the executable for its own branch/commit/version.

    `hft_app --branch --commit --version` prints labeled key=value lines
    and exits WITHOUT trading -- the flag gate in src/app/main.cpp runs
    before logging, config load and broker connect. This is the
    authoritative source of provenance: values compiled into the image
    cannot be separated from the image, whereas bin/binary.json
    described the binary from the outside and could (and did) drift.

    Returns {} when the binary cannot answer, so callers fall back to
    the legacy manifest.
    """
    if not _supports_provenance_flags(exe):
        return {}
    try:
        res = subprocess.run(
            [str(exe), "--branch", "--commit", "--version"],
            capture_output=True, text=True, timeout=10, check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return {}
    if res.returncode != 0:
        return {}
    info: Dict[str, Any] = {}
    for line in res.stdout.splitlines():
        key, sep, val = line.partition("=")
        if sep and key in ("branch", "commit", "version"):
            info[key] = val.strip()
    return info


def _supports_provenance_flags(exe: Path) -> bool:
    """True if this binary understands --branch/--commit/--version.

    SAFETY CRITICAL. A pre-provenance hft_app was `int main()` and
    ignored argv completely, so invoking it with flags would not print
    anything -- it would START A TRADING SESSION. We therefore look for
    the usage string inside the image before ever exec'ing it with
    arguments. Only binaries that demonstrably contain the flag-handling
    code get run.
    """
    try:
        with exe.open("rb") as f:
            return b"usage: hft_app [--branch]" in f.read()
    except OSError:
        return False


def _list_binaries(inst: Optional[str] = None) -> List[Dict[str, Any]]:
    """Enumerates runnable binaries: the default bin/hft_app plus every
    bin/versions/<version>/.

    Provenance comes from the binary itself (see
    _probe_binary_provenance). bin/binary.json is consulted only as a
    fallback for binaries built before the flags existed; the response
    carries provenance_source so the app can tell the two apart.
    """
    out: List[Dict[str, Any]] = []
    bin_dir = (_instance_dir(inst) if inst else REPO_ROOT) / "bin"

    def _manifest(d: Path) -> Dict[str, Any]:
        mf = d / "binary.json"
        if mf.is_file():
            try:
                return json.loads(mf.read_text())
            except Exception:
                return {}
        return {}

    def _describe(exe: Path, manifest_dir: Path) -> Dict[str, Any]:
        probed = _probe_binary_provenance(exe)
        if probed:
            return {
                "branch": probed.get("branch"),
                "commit": probed.get("commit"),
                "build_version": probed.get("version"),
                "built_at": None,
                "description": None,
                "provenance_source": "binary",
            }
        mf = _manifest(manifest_dir)
        return {
            "branch": mf.get("branch"),
            "commit": mf.get("commit"),
            "build_version": None,
            "built_at": mf.get("built_at"),
            "description": mf.get("description"),
            "provenance_source": "manifest" if mf else "unknown",
        }

    # The current default binary. Resolve the symlink so the app can see
    # which staged version bin/hft_app actually points at.
    default_bin = bin_dir / "hft_app"
    if default_bin.exists():
        info = _describe(default_bin, bin_dir)
        target = None
        try:
            if default_bin.is_symlink():
                target = os.path.basename(
                    os.path.dirname(os.path.realpath(default_bin)))
        except OSError:
            target = None
        out.append({
            "version": "current",
            "is_default": True,
            "points_to": target,
            "description": info["description"] or "default bin/hft_app",
            "branch": info["branch"],
            "commit": info["commit"],
            "build_version": info["build_version"],
            "built_at": info["built_at"],
            "provenance_source": info["provenance_source"],
            "config_schema": _config_schema_for_branch(info["branch"]),
        })

    versions_dir = bin_dir / "versions"
    if versions_dir.is_dir():
        for d in sorted(versions_dir.iterdir(), reverse=True):
            exe = d / "hft_app"
            if not d.is_dir() or not exe.exists():
                continue
            info = _describe(exe, d)
            out.append({
                "version": d.name,
                "is_default": False,
                "points_to": None,
                "description": info["description"] or d.name,
                "branch": info["branch"],
                "commit": info["commit"],
                "build_version": info["build_version"],
                "built_at": info["built_at"],
                "provenance_source": info["provenance_source"],
                "config_schema": _config_schema_for_branch(info["branch"]),
            })
    return out


@app.get("/binaries")
def list_binaries(req: Request, instance: Optional[str] = None):
    """Runnable binaries + each one's branch and config schema, so the
    app can offer branch selection and show only the configs that branch
    supports."""
    _require_token(req)
    inst = _resolve_instance(instance)
    return {"instance": inst, "binaries": _list_binaries(inst)}


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
    """Start an instance.

    Body:
      {
        "instance": "paper" | "live",     # "mode" accepted as an alias
        "confirm": true,                  # required
        "confirm_live": true,             # required for the live instance
        "binary_version": "..."           # optional; repoints bin/hft_app
      }

    Selecting an instance STARTS A DIFFERENT UNIT against a different
    directory; it no longer rewrites any config. Each instance's mode
    is fixed in its own config.ini, so paper cannot be talked into
    trading live.

    Refuses if that instance's IB Gateway port is not up, or if that
    instance is already running.
    """
    _require_token(req)
    # "mode" is the old field name and meant the same thing to callers.
    inst = _resolve_instance(payload.get("instance") or payload.get("mode"))
    if not payload.get("confirm"):
        raise HTTPException(status_code=400, detail="confirm=true required")
    if inst == "live" and not payload.get("confirm_live"):
        raise HTTPException(
            status_code=400,
            detail="confirm_live=true required to start LIVE trading",
        )

    inst_dir = _instance_dir(inst)
    if not inst_dir.is_dir():
        raise HTTPException(status_code=404,
                            detail=f"no such instance directory: {inst_dir}")

    # The instance's own config decides the mode. Cross-check it against
    # the instance name so a mislabelled config cannot route live
    # trading through the paper instance.
    mode = _instance_mode(inst)
    expected = "live" if inst == "live" else "ibkr_paper"
    if mode != expected:
        raise HTTPException(
            status_code=500,
            detail=f"instance '{inst}' has mode={mode!r}, expected "
                   f"{expected!r}; refusing to start a mislabelled instance",
        )

    # Refuse to double-start THIS instance. Checked per instance so
    # paper running does not block live, or vice versa.
    running = subprocess.run(
        ["pgrep", "-f", _instance_pattern(inst)],
        capture_output=True, text=True, check=False,
    ).stdout.strip()
    if running:
        raise HTTPException(status_code=409,
                            detail=f"{inst} engine already running")

    port = 4001 if inst == "live" else 4002
    if not _gateway_reachable(port):
        raise HTTPException(
            status_code=503,
            detail=f"IB Gateway not reachable on 127.0.0.1:{port} "
                   f"(instance={inst}); is hft_ibgateway up and logged in?",
        )

    # Optional binary swap, within THIS instance's own bin/.
    version = payload.get("binary_version")
    if version and version != "current":
        target = inst_dir / "bin" / "versions" / version / "hft_app"
        if not target.exists():
            raise HTTPException(
                status_code=404,
                detail=f"binary_version {version} not found for {inst}")
        link = inst_dir / "bin" / "hft_app"
        try:
            if link.is_symlink() or link.exists():
                link.unlink()
            link.symlink_to(Path("versions") / version / "hft_app")
        except Exception as exc:
            raise HTTPException(status_code=500,
                                detail=f"binary swap failed: {exc}")

    unit = _instance_unit(inst)
    try:
        subprocess.run(["systemctl", "start", unit],
                       capture_output=True, text=True, check=True)
    except subprocess.CalledProcessError as exc:
        raise HTTPException(status_code=500,
                            detail=f"systemctl start failed: {exc.stderr}")
    return {"started": True, "instance": inst, "mode": mode, "unit": unit,
            "port": port, "binary_version": version or "current"}


@app.post("/live/stop")
def live_stop(payload: Optional[Dict[str, Any]] = None,
              req: Request = None):
    """Stop one instance. Body: {"instance": "paper"|"live"}.

    For an emergency freeze while keeping the process up, use /kill.
    """
    _require_token(req)
    payload = payload or {}
    inst = _resolve_instance(payload.get("instance") or payload.get("mode"))
    unit = _instance_unit(inst)
    try:
        subprocess.run(["systemctl", "stop", unit],
                       capture_output=True, text=True, check=True)
    except subprocess.CalledProcessError as exc:
        raise HTTPException(status_code=500,
                            detail=f"systemctl stop failed: {exc.stderr}")
    return {"stopped": True, "instance": inst, "unit": unit}


# ------------------------------------------------- live orders/positions

def _config_map(inst: Optional[str] = None) -> Dict[str, str]:
    """Flat key=value view of an instance's config.ini (section headers
    ignored, which matches how the C++ AppConfig parses it)."""
    out: Dict[str, str] = {}
    root = _instance_dir(inst) if inst else REPO_ROOT
    cfg_path = root / "config.ini"
    if cfg_path.is_file():
        for line in cfg_path.read_text().splitlines():
            s = line.strip()
            if s and not s.startswith("#") and not s.startswith("[") and "=" in s:
                k, v = s.split("=", 1)
                out[k.strip()] = v.strip()
    return out


def _read_csv_rows(path: Path) -> List[Dict[str, str]]:
    """Parses one of the engine's CSVs, tolerating the `# session_*`
    comment markers and a header line that repeats after each marker."""
    import csv
    import io
    if not path.is_file():
        return []
    body = "".join(
        ln for ln in path.read_text().splitlines(keepends=True)
        if not ln.startswith("#")
    )
    if not body.strip():
        return []
    reader = csv.DictReader(io.StringIO(body))
    rows: List[Dict[str, str]] = []
    for r in reader:
        if r.get("ts_ns") == "ts_ns":   # repeated header
            continue
        rows.append(r)
    return rows


def _latest_mid_by_symbol(path: Path) -> Dict[str, float]:
    out: Dict[str, float] = {}
    for r in _read_csv_rows(path):
        sym = r.get("symbol")
        try:
            out[sym] = float(r["mid"])
        except (KeyError, ValueError, TypeError):
            continue
    return out


# --- IBKR snapshot -----------------------------------------------------
#
# /live/orders used to be derived entirely from reports/orders.csv and
# reports/decisions.csv. The Chronos engine writes NEITHER -- the config
# still carries order_log_path and decision_log_path, but nothing on
# this branch reads them; they belonged to LiveExecutionEngine, which
# was deleted. So the screen was empty while the paper account held six
# positions worth real money.
#
# The broker is the only source of truth for positions and orders, so
# ask it. Via a subprocess: ib_insync drives its own asyncio loop and
# its sync API deadlocks inside uvicorn's running loop, and a hung
# gateway must not be able to wedge the API.
_SNAPSHOT_TTL_SEC = 5.0
_snapshot_cache: Dict[str, Any] = {"at": 0.0, "data": None}


def _ibkr_snapshot(port: int) -> Dict[str, Any]:
    """Positions, working orders and P&L straight from IBKR.

    Cached briefly: the app polls this screen, and each call is a fresh
    gateway connection.
    """
    now = time.time()
    cached = _snapshot_cache.get("data")
    if cached is not None and (now - _snapshot_cache["at"]) < _SNAPSHOT_TTL_SEC:
        return cached

    script = REPO_ROOT.parent / "services" / "scripts" / "ibkr_snapshot.py"
    if not script.is_file():
        script = Path(__file__).resolve().parent.parent / "ibkr_snapshot.py"
    venv_py = REPO_ROOT.parent / "services" / ".venv" / "bin" / "python"
    interp = str(venv_py) if venv_py.exists() else sys.executable
    try:
        out = subprocess.run(
            [interp, str(script), "--port", str(port)],
            capture_output=True, text=True, timeout=40, check=False,
        )
        data = json.loads(out.stdout or "{}")
    except Exception as exc:
        data = {"ok": False, "positions": [], "orders": [], "account": {},
                "error": f"snapshot failed: {str(exc)[:160]}"}

    _snapshot_cache["at"] = now
    _snapshot_cache["data"] = data
    return data


# --- CI image catalogue ------------------------------------------------
#
# CI publishes an image per commit to ghcr as <branch>-<shortsha>. The
# registry knows which builds EXIST; only git knows what they CONTAIN.
# Joining the two is what makes a tag list usable by a human -- a column
# of hashes is not a thing anyone can choose from.
#
# The package is public, so an anonymous pull token is enough and no PAT
# has to live on the box.
GHCR_IMAGE = os.environ.get(
    "HFT_GHCR_IMAGE", "munteanu-mihai-alin/trading-system/hft_app"
)
_IMAGES_TTL_SEC = 60.0
_images_cache: Dict[str, Any] = {"at": 0.0, "data": None}


def _ghcr_tags() -> List[str]:
    """Every tag published for the image, or [] if the registry is unreachable."""
    import urllib.request

    def _get(url: str, headers: Dict[str, str]) -> Dict[str, Any]:
        rq = urllib.request.Request(url, headers=headers)
        with urllib.request.urlopen(rq, timeout=15) as r:
            return json.loads(r.read().decode("utf-8"))

    try:
        tok = _get(
            f"https://ghcr.io/token?scope=repository:{GHCR_IMAGE}:pull", {}
        )["token"]
        return _get(
            f"https://ghcr.io/v2/{GHCR_IMAGE}/tags/list",
            {"Authorization": f"Bearer {tok}"},
        ).get("tags", []) or []
    except Exception:
        return []


def _git_commit_meta(shas: List[str]) -> Dict[str, Dict[str, Any]]:
    """subject + author date for each sha, from the services checkout.

    A sha the local clone has never fetched simply gets no entry; the
    caller shows the tag without a message rather than hiding the image.
    """
    out: Dict[str, Dict[str, Any]] = {}
    repo = Path(__file__).resolve().parents[2]
    for sha in shas:
        try:
            r = subprocess.run(
                ["git", "-C", str(repo), "show", "-s",
                 "--format=%H%x1f%s%x1f%aI%x1f%an", sha],
                capture_output=True, text=True, timeout=10, check=False,
            )
            if r.returncode != 0 or not r.stdout.strip():
                continue
            full, subject, authored, author = r.stdout.strip().split("\x1f")
            out[sha] = {"commit_full": full, "subject": subject,
                        "authored_at": authored, "author": author}
        except Exception:
            continue
    return out


@app.get("/images")
def list_images(req: Request, instance: Optional[str] = None,
                limit: int = 25):
    """CI-built images for this branch, newest first.

    Each entry carries the commit message and date so a build can be
    chosen by what it changed rather than by hash. deployed=true marks
    the one the instance is currently running.
    """
    _require_token(req)
    inst = _resolve_instance(instance)

    now = time.time()
    cached = _images_cache.get("data")
    if cached is None or (now - _images_cache["at"]) >= _IMAGES_TTL_SEC:
        cached = _ghcr_tags()
        _images_cache["at"] = now
        _images_cache["data"] = cached
    tags = cached

    # What this instance runs right now, so the app can mark it.
    running_commit = None
    exe = _instance_dir(inst) / "bin" / "hft_app"
    probed = _probe_binary_provenance(exe)
    if probed:
        running_commit = probed.get("commit")

    # Tags are "<branch>-<shortsha>"; the bare branch tag is a moving
    # pointer at the newest build and would duplicate a pinned entry.
    entries = []
    seen = set()
    for tag in tags:
        if "-" not in tag:
            continue
        sha = tag.rsplit("-", 1)[1]
        if len(sha) < 6 or not all(c in "0123456789abcdef" for c in sha):
            continue
        if sha in seen:
            continue
        seen.add(sha)
        entries.append({"tag": tag, "commit": sha})

    meta = _git_commit_meta([e["commit"] for e in entries])
    for e in entries:
        m = meta.get(e["commit"], {})
        e["subject"] = m.get("subject")
        e["authored_at"] = m.get("authored_at")
        e["author"] = m.get("author")
        e["deployed"] = (e["commit"] == running_commit)

    # Newest first. Commits we have metadata for sort by date; anything
    # unknown goes last rather than being silently dropped.
    entries.sort(key=lambda e: (e["authored_at"] or ""), reverse=True)

    return {
        "instance": inst,
        "image": GHCR_IMAGE,
        "running_commit": running_commit,
        "images": entries[: max(1, min(limit, 100))],
    }


@app.post("/images/deploy")
def deploy_image(payload: Dict[str, Any], req: Request):
    """Deploy a CI image to an instance.

    Body: {"instance": "paper", "tag": "<branch>-<sha>", "force": false}

    Delegates to scripts/deploy_from_ghcr.sh, which owns the safety
    rules: it refuses while that instance's engine is running (not
    overridable), refuses inside the trading window unless forced,
    verifies the pulled binary runs and reports the commit it claims,
    and rolls the symlink back if the post-swap check fails.
    """
    _require_token(req)
    inst = _resolve_instance(payload.get("instance"))
    tag = str(payload.get("tag") or "").strip()
    if not tag:
        raise HTTPException(status_code=400, detail="tag is required")
    # The tag becomes a docker ref and a directory name.
    if not all(c.isalnum() or c in "-._" for c in tag):
        raise HTTPException(status_code=400, detail="invalid tag")

    script = Path(__file__).resolve().parents[1] / "deploy_from_ghcr.sh"
    cmd = ["bash", str(script), "--instance", inst, "--tag", tag]
    if payload.get("force"):
        cmd.append("--force")
    try:
        r = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=600, check=False)
    except subprocess.TimeoutExpired:
        raise HTTPException(status_code=504, detail="deploy timed out")

    ok = r.returncode == 0
    return {
        "ok": ok,
        "instance": inst,
        "tag": tag,
        "returncode": r.returncode,
        # Both streams matter: the interlocks explain themselves on
        # stderr, and a refusal is a normal outcome, not a crash.
        "stdout": r.stdout[-4000:],
        "stderr": r.stderr[-4000:],
    }


@app.get("/live/orders")
def live_orders(req: Request, instance: Optional[str] = None):
    """Positions, working exit orders and P&L for one instance.

    Sourced from IBKR, not from the engine's CSVs -- the Chronos engine
    writes none (see _ibkr_snapshot). target_price is the resting sell's
    limit when one exists, otherwise entry * (1 + target_profit_pct),
    which is what route_exit_orders would place.
    """
    _require_token(req)
    inst = _resolve_instance(instance)
    cfg = _config_map(inst)
    port = 4001 if inst == "live" else 4002

    snap = _ibkr_snapshot(port)
    try:
        target_pct = float(cfg.get("target_profit_pct", "0.008"))
    except ValueError:
        target_pct = 0.008

    # Working sells, keyed by symbol, so a position can show the limit
    # actually resting at the broker rather than a computed guess.
    sell_limit_by_symbol: Dict[str, float] = {}
    orders_out = []
    for o in snap.get("orders", []):
        sym = o.get("symbol")
        if o.get("side") == "SELL" and sym and o.get("limit"):
            sell_limit_by_symbol[sym] = float(o["limit"])
        orders_out.append({
            "ts": None,
            "order_id": str(o.get("order_id")) if o.get("order_id") else None,
            "symbol": sym,
            "side": (o.get("side") or "").lower() or None,
            "event": "working",
            "qty": o.get("qty"),
            "limit": o.get("limit"),
            "fill_price": None,
        })

    open_positions = []
    unrealized_total = 0.0
    for p in snap.get("positions", []):
        qty = float(p.get("qty") or 0.0)
        if qty <= 0.0:
            continue
        entry = float(p.get("avg_cost") or 0.0)
        mkt = float(p.get("market_price") or 0.0)
        unrl = float(p.get("unrealized") or 0.0)
        unrealized_total += unrl
        target = sell_limit_by_symbol.get(
            p["symbol"], entry * (1.0 + target_pct))
        upside = None
        if mkt > 0.0 and target > 0.0:
            upside = (target - mkt) / mkt * 100.0
        open_positions.append({
            "symbol": p["symbol"],
            "qty": qty,
            "entry_price": entry,
            "target_price": target,
            "predicted_upside_pct": upside,
            "last_mid": mkt or None,
            "unrealized": unrl,
        })
    open_positions.sort(key=lambda x: x["symbol"])

    def _acct(tag: str) -> float:
        try:
            return float(snap.get("account", {}).get(tag, {}).get("USD", 0.0))
        except (TypeError, ValueError):
            return 0.0

    realized = _acct("RealizedPnL")
    return {
        "stats": {
            "realized_pnl": realized,
            "unrealized_pnl": unrealized_total,
            "net_pnl": realized + unrealized_total,
            "open_positions": len(open_positions),
            "closed_round_trips": 0,
            "win_rate": None,
            "filled_buys": 0,
            "filled_sells": 0,
            "total_orders": len(orders_out),
        },
        "open_positions": open_positions,
        "orders": orders_out,
        "source": "ibkr",
        "snapshot_ok": bool(snap.get("ok")),
        "snapshot_error": snap.get("error"),
    }



def live_orders(req: Request, instance: Optional[str] = None):
    """Live/paper session orders, open positions (with the target/predicted
    exit price), and a small stats block derived from the engine's
    order + decision logs.

    open_positions[].target_price is the model's predicted exit: the
    limit of the resting sell for that symbol when one exists (that is
    the Chronos-2 predicted price, or the OU/target the Hawkes engine
    computed), else entry * (1 + target_profit_pct).
    """
    _require_token(req)
    inst = _resolve_instance(instance)
    inst_dir = _instance_dir(inst)
    cfg = _config_map(inst)
    orders_path = inst_dir / cfg.get("order_log_path", "reports/orders.csv")
    dec_path = inst_dir / cfg.get("decision_log_path", "reports/decisions.csv")
    target_pct = float(cfg.get("target_profit_pct", 0.008))
    per_share = float(cfg.get("commission_per_share", 0.0035))
    min_order = float(cfg.get("commission_min_per_order", 0.35))

    rows = _read_csv_rows(orders_path)

    def _ts(r: Dict[str, str]) -> int:
        try:
            return int(r["ts_ns"])
        except (KeyError, ValueError):
            return 0

    def _iso(ns: int) -> Optional[str]:
        if ns <= 0:
            return None
        import datetime as _dt
        return _dt.datetime.fromtimestamp(
            ns / 1e9, tz=_dt.timezone.utc
        ).strftime("%Y-%m-%dT%H:%M:%SZ")

    # Latest sell-order limit per symbol = the predicted/target exit.
    sell_limit: Dict[str, float] = {}
    for r in rows:
        if r.get("side") == "sell":
            try:
                sell_limit[r["symbol"]] = float(r["limit"])
            except (KeyError, ValueError):
                pass

    filled = sorted(
        (r for r in rows if r.get("event") == "filled"), key=_ts
    )
    open_by_symbol: Dict[str, Dict[str, float]] = {}
    realized = 0.0
    n_closed = 0
    wins = 0
    for r in filled:
        sym, side = r.get("symbol"), r.get("side")
        try:
            qty = float(r["filled_qty"])
            px = float(r["avg_fill_price"])
        except (KeyError, ValueError):
            continue
        if side == "buy":
            open_by_symbol[sym] = {"qty": qty, "entry": px}
        elif side == "sell" and sym in open_by_symbol:
            e = open_by_symbol.pop(sym)
            comm = 2 * max(per_share * e["qty"], min_order)
            pnl = (px - e["entry"]) * e["qty"] - comm
            realized += pnl
            n_closed += 1
            if pnl > 0:
                wins += 1

    last_mid = _latest_mid_by_symbol(dec_path)
    open_positions = []
    unrealized_total = 0.0
    for sym, e in open_by_symbol.items():
        target = sell_limit.get(sym, e["entry"] * (1.0 + target_pct))
        mid = last_mid.get(sym)
        unreal = (mid - e["entry"]) * e["qty"] if mid is not None else None
        if unreal is not None:
            unrealized_total += unreal
        open_positions.append({
            "symbol": sym,
            "qty": e["qty"],
            "entry_price": round(e["entry"], 4),
            "target_price": round(target, 4),
            "predicted_upside_pct": round(
                (target - e["entry"]) / e["entry"] * 100.0, 3
            ) if e["entry"] else None,
            "last_mid": round(mid, 4) if mid is not None else None,
            "unrealized": round(unreal, 2) if unreal is not None else None,
        })
    open_positions.sort(key=lambda p: p["symbol"])

    recent = []
    for r in sorted(rows, key=_ts, reverse=True)[:40]:
        try:
            recent.append({
                "ts": _iso(_ts(r)),
                "order_id": r.get("order_id"),
                "symbol": r.get("symbol"),
                "side": r.get("side"),
                "event": r.get("event"),
                "qty": float(r["qty"]) if r.get("qty") else None,
                "limit": float(r["limit"]) if r.get("limit") else None,
                "fill_price": float(r["avg_fill_price"])
                if r.get("avg_fill_price") not in (None, "", "0")
                else None,
            })
        except (ValueError, TypeError):
            continue

    n_buys = sum(1 for r in filled if r.get("side") == "buy")
    n_sells = sum(1 for r in filled if r.get("side") == "sell")
    return {
        "stats": {
            "realized_pnl": round(realized, 2),
            "unrealized_pnl": round(unrealized_total, 2),
            "net_pnl": round(realized + unrealized_total, 2),
            "open_positions": len(open_positions),
            "closed_round_trips": n_closed,
            "win_rate": round(wins / n_closed, 4) if n_closed else None,
            "filled_buys": n_buys,
            "filled_sells": n_sells,
            "total_orders": len(rows),
        },
        "open_positions": open_positions,
        "orders": recent,
    }




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
