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
    """Launch a new backtest. Mirrors scripts/hft_backtest.sh flags.

    Body:
      {
        "config": "config.databento_backtest.yen.ini",
        "label":  "yen_v5",
        "target_profit_pct": 0.008,
        "start":  "2024-08-02T13:30:00Z",
        "end":    "2024-08-09T20:00:00Z",
        "symbols": "config/symbols_yen.txt"
      }
    All keys optional except `config`.

    Forks a `scripts/hft_backtest.sh` invocation under systemd-run so
    the supervisor sees it. Returns the systemd-run unit name; the
    operator (or future GET /backtests endpoint) polls journalctl for
    status.
    """
    _require_token(req)
    cfg = payload.get("config")
    if not cfg:
        raise HTTPException(status_code=400, detail="config is required")
    args = [str(REPO_ROOT / "scripts" / "hft_backtest.sh"), "--config", cfg]
    for k_in, k_out in (
        ("target_profit_pct", "--target"),
        ("label", "--label"),
        ("start", "--start"),
        ("end", "--end"),
        ("symbols", "--symbols"),
    ):
        v = payload.get(k_in)
        if v is not None:
            args += [k_out, str(v)]
    # Spawn via systemd-run so the backtest runs as a transient unit
    # the supervisor can monitor independently. --collect cleans up
    # the unit on success.
    unit_name = f"hft-backtest-{payload.get('label', 'unnamed')}"
    sysrun_cmd = [
        "systemd-run", "--user=root", "--unit", unit_name,
        "--working-directory", str(REPO_ROOT), "--collect",
        *args,
    ]
    try:
        out = subprocess.run(
            sysrun_cmd, capture_output=True, text=True, check=False,
        )
    except FileNotFoundError:
        # systemd-run not present (e.g. local dev). Fall back to plain
        # subprocess so the endpoint is at least testable on a
        # developer laptop.
        out = subprocess.Popen(args, cwd=str(REPO_ROOT))
        return {"unit": None, "pid": out.pid, "transient": False}
    return {
        "unit": unit_name,
        "stdout": out.stdout,
        "stderr": out.stderr,
        "returncode": out.returncode,
    }


@app.get("/backtests")
def list_backtests(req: Request):
    """List currently-running transient backtest units."""
    _require_token(req)
    try:
        out = subprocess.run(
            ["systemctl", "list-units", "--type=service", "--all",
             "--plain", "--no-legend", "hft-backtest-*"],
            capture_output=True, text=True, check=False,
        )
        if out.returncode != 0:
            return {"units": []}
        units = []
        for line in out.stdout.splitlines():
            parts = line.split()
            if len(parts) >= 4:
                units.append({
                    "name": parts[0],
                    "load": parts[1],
                    "active": parts[2],
                    "sub": parts[3],
                })
        return {"units": units}
    except FileNotFoundError:
        return {"units": [], "reason": "systemctl not available"}


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
