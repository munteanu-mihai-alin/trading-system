#!/usr/bin/env python3
"""Background watchdog for the live HFT engine on Hetzner.

Polls every N seconds and calls scripts/notify.sh on threshold
breaches. Thresholds are conservative; tune via env vars (the
hft_monitor.service file sources /etc/hft/monitor.env).

What's checked each tick:
  - Disk free on /mnt/HC_Volume_105581071  (default alert < 20 GB)
  - System mem free                         (default alert < 1 GB)
  - hft_app process RSS                    (default alert > 12 GB)
  - hft_app liveness                        (alert immediately if
    expected_running=true and we don't find the pid)

State (last-alert timestamps per metric) is held in /var/run/hft_monitor.state
so a flapping condition doesn't spam the operator -- each metric
emits at most one alert per ALERT_COOLDOWN_SEC window.

Usage:
  scripts/hft_monitor.py [--once] [--config /etc/hft/monitor.env]

Designed to run as a long-lived systemd service (see
scripts/systemd/hft_monitor.service); the --once flag is for manual
testing.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Dict, Optional


DEFAULT_CONFIG = {
    "VOLUME_PATH": "/mnt/HC_Volume_105581071",
    "DISK_FREE_ALERT_GB": "20",
    "MEM_FREE_ALERT_GB": "1",
    "RSS_ALERT_GB": "12",
    "POLL_SEC": "30",
    "ALERT_COOLDOWN_SEC": "1800",  # 30 min between repeat alerts per metric
    "NOTIFY_SCRIPT": "/mnt/HC_Volume_105581071/trading-live/services/scripts/notify.sh",
    "STATE_FILE": "/var/run/hft_monitor.state",
    "HFT_APP_PATTERN": "bin/hft_app",
    # "Engine up but blind" detection. The engine publishes MarketData
    # transitions into its own log; if it is running during the regular
    # session and market data has been Down for longer than this, the
    # session is silently producing nothing. That is the failure mode
    # that a competing IBKR login (error 10197) or a lapsed market-data
    # subscription (10089) produces: the engine connects, reports
    # broker=Ready, and simply never receives a quote.
    "MD_BLIND_ALERT_SEC": "300",
    "ENGINE_LOG": "/mnt/HC_Volume_105581071/trading-live/paper/logs/hft_app.log",
    # The instance's bin/hft_app symlink. Compared against what the
    # running process actually has mapped, to catch "deployed but never
    # restarted" -- the engine keeps running the old binary while every
    # other signal (symlink, binary.json, GET /binaries) reports the new
    # one, so the box looks upgraded and is not.
    "ENGINE_BIN": "/mnt/HC_Volume_105581071/trading-live/paper/bin/hft_app",
    # When true, an absent hft_app process triggers an alert. Disable
    # while we're outside RTH or doing maintenance.
    "EXPECT_RUNNING": "false",
    # Sibling daemons we want to know are up. Each is a systemd unit
    # name; the monitor calls `systemctl is-active <unit>` once per
    # tick and alerts when the status changes to "failed" / "inactive".
    "SIBLING_UNITS": "hft_backend.service,hft_backtest_launcher.service",
    # The launcher writes its own liveness state here; if the file is
    # older than this many seconds the monitor treats it as wedged
    # (different from "exited" -- the unit might be up but the loop
    # might be hung).
    "LAUNCHER_STATE_FILE": "/var/run/hft_backtest_launcher.state",
    "LAUNCHER_STATE_STALE_SEC": "300",
}


def load_config(path: Optional[Path]) -> Dict[str, str]:
    cfg = dict(DEFAULT_CONFIG)
    if path and path.is_file():
        for line in path.read_text().splitlines():
            t = line.strip()
            if not t or t.startswith("#"):
                continue
            if "=" in t:
                k, v = t.split("=", 1)
                cfg[k.strip()] = v.strip().strip('"').strip("'")
    # Env vars override file values (lets the systemd unit inject).
    for k in cfg:
        if k in os.environ:
            cfg[k] = os.environ[k]
    return cfg


def disk_free_gb(path: str) -> float:
    try:
        stat = shutil.disk_usage(path)
        return stat.free / (1024 ** 3)
    except Exception:
        return float("nan")


def mem_free_gb() -> float:
    # Read /proc/meminfo so we don't depend on psutil.
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemAvailable:"):
                    kb = int(line.split()[1])
                    return kb / (1024 ** 2)
    except Exception:
        pass
    return float("nan")


def hft_app_rss_gb(pattern: str) -> Optional[float]:
    """Returns RSS in GB or None if the process isn't running."""
    try:
        # pgrep -fa returns "pid full-command"; -o picks the oldest.
        out = subprocess.check_output(
            ["pgrep", "-fao", pattern],
            stderr=subprocess.DEVNULL,
            text=True,
        ).strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None
    if not out:
        return None
    pid = out.split()[0]
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    kb = int(line.split()[1])
                    return kb / (1024 ** 2)
    except Exception:
        return None
    return None


class State:
    """Persisted last-alert timestamps so we throttle repeats."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.data: Dict[str, float] = {}
        if path.exists():
            try:
                self.data = json.loads(path.read_text())
            except Exception:
                self.data = {}

    def can_alert(self, key: str, cooldown_sec: int) -> bool:
        last = self.data.get(key, 0.0)
        return (time.time() - last) >= cooldown_sec

    def mark_alerted(self, key: str) -> None:
        self.data[key] = time.time()
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            self.path.write_text(json.dumps(self.data))
        except Exception:
            pass


def notify(script: str, message: str, tag: str) -> None:
    """Best-effort: failures are logged but don't crash the monitor."""
    try:
        subprocess.run(
            [script, message, tag],
            timeout=15,
            check=False,
        )
    except Exception as exc:
        print(f"hft_monitor: notify failed: {exc}", file=sys.stderr)


def systemd_is_active(unit: str) -> str:
    """Returns systemctl's textual is-active status (active / inactive /
    failed / activating / unknown). Empty string when systemctl is
    unavailable (dev box).
    """
    try:
        out = subprocess.run(
            ["systemctl", "is-active", unit],
            capture_output=True, text=True, check=False,
        )
        return out.stdout.strip()
    except FileNotFoundError:
        return ""


def launcher_state_age_sec(path: str) -> Optional[float]:
    """Returns seconds since the launcher's state file was last
    written, or None when the file doesn't exist (launcher never
    started in this session)."""
    p = Path(path)
    if not p.exists():
        return None
    try:
        return time.time() - p.stat().st_mtime
    except Exception:
        return None


def hft_app_pid(pattern: str) -> Optional[int]:
    """Pid of the running engine, or None."""
    try:
        out = subprocess.run(
            ["pgrep", "-f", pattern],
            capture_output=True, text=True, check=False,
        )
        pids = [p for p in out.stdout.split() if p.isdigit()]
        return int(pids[0]) if pids else None
    except (OSError, ValueError):
        return None


def running_binary_mismatch(pid: int, link_path: str) -> Optional[str]:
    """Describe a running-vs-deployed binary mismatch, or None if they agree.

    /proc/<pid>/exe resolves to the file the process actually mapped, so
    it keeps pointing at the old version directory after a deploy
    repoints the symlink -- which is precisely the condition worth
    reporting. A replaced-in-place file shows up as "(deleted)".

    Returns None on any resolution failure: a mismatch we cannot prove
    is not one we should wake somebody for.
    """
    try:
        running = os.path.realpath("/proc/%d/exe" % pid)
        deployed = os.path.realpath(link_path)
    except OSError:
        return None
    if not running or not deployed:
        return None
    # pgrep -f matches any command line containing the pattern,
    # including shells and scripts that merely mention the path. If
    # the resolved executable is not an hft_app at all then we found
    # the wrong process, and an alert would be pure noise.
    if "hft_app" not in os.path.basename(running):
        return None
    if running.endswith(" (deleted)"):
        return "running binary was replaced on disk (%s)" % running
    if running != deployed:
        return "running %s but %s points at %s" % (
            os.path.basename(os.path.dirname(running)),
            link_path,
            os.path.basename(os.path.dirname(deployed)),
        )
    return None


def is_rth_now() -> bool:
    """True during the NYSE regular session (09:30-16:00 America/New_York).

    Mirrors include/app/trading_hours.hpp. Kept as a duplicate rather
    than shared because the monitor must keep working even when the
    engine is not running -- that is precisely when it has something
    to say.
    """
    try:
        from datetime import datetime
        from zoneinfo import ZoneInfo
        now = datetime.now(ZoneInfo("America/New_York"))
    except Exception:
        return False          # fail closed: no tz data -> do not alert
    if now.weekday() >= 5:
        return False
    minutes = now.hour * 60 + now.minute
    return (9 * 60 + 30) <= minutes < (16 * 60)


def md_down_seconds(log_path: str) -> Optional[float]:
    """Seconds since MarketData last went Down, or None if it is Ready.

    Reads the tail of the engine log for the most recent MarketData
    transition. The engine publishes these only on change (a no-op
    publish every step would be ~94k lines a session), so the last
    matching line is the current state.

    Returns None when the state is Ready, when no transition has been
    logged yet, or when the log cannot be read -- all "nothing to say"
    rather than "raise an alarm".
    """
    try:
        with open(log_path, "rb") as f:
            try:
                f.seek(-200_000, os.SEEK_END)
            except OSError:
                f.seek(0)
            tail = f.read().decode("utf-8", "replace").splitlines()
    except OSError:
        return None

    for line in reversed(tail):
        if "MarketData" not in line:
            continue
        if "-> Ready" in line:
            return None
        if "-> Down" in line:
            try:
                stamp = line.split("[", 1)[1].split("]", 1)[0]
                from datetime import datetime
                when = datetime.strptime(stamp, "%Y-%m-%d %H:%M:%S.%f")
                return max(0.0, time.time() - when.timestamp())
            except Exception:
                return None
    return None


def check_once(cfg: Dict[str, str], state: State) -> None:
    cooldown = int(cfg["ALERT_COOLDOWN_SEC"])
    notify_script = cfg["NOTIFY_SCRIPT"]

    # Disk
    free_gb = disk_free_gb(cfg["VOLUME_PATH"])
    if free_gb == free_gb and free_gb < float(cfg["DISK_FREE_ALERT_GB"]):
        if state.can_alert("disk", cooldown):
            notify(
                notify_script,
                f"DISK low on {cfg['VOLUME_PATH']}: "
                f"{free_gb:.1f} GB free (threshold "
                f"{cfg['DISK_FREE_ALERT_GB']} GB)",
                "error",
            )
            state.mark_alerted("disk")

    # Mem
    mem_gb = mem_free_gb()
    if mem_gb == mem_gb and mem_gb < float(cfg["MEM_FREE_ALERT_GB"]):
        if state.can_alert("mem", cooldown):
            notify(
                notify_script,
                f"MEM low: {mem_gb:.2f} GB free (threshold "
                f"{cfg['MEM_FREE_ALERT_GB']} GB)",
                "error",
            )
            state.mark_alerted("mem")

    # hft_app RSS + liveness
    rss = hft_app_rss_gb(cfg["HFT_APP_PATTERN"])
    expect_running = cfg["EXPECT_RUNNING"].lower() in ("1", "true", "yes")
    if rss is None:
        if expect_running and state.can_alert("not_running", cooldown):
            notify(
                notify_script,
                "hft_app not running but EXPECT_RUNNING=true",
                "crash",
            )
            state.mark_alerted("not_running")
    else:
        if rss > float(cfg["RSS_ALERT_GB"]):
            if state.can_alert("rss", cooldown):
                notify(
                    notify_script,
                    f"hft_app RSS {rss:.1f} GB exceeds threshold "
                    f"{cfg['RSS_ALERT_GB']} GB",
                    "error",
                )
                state.mark_alerted("rss")

    # Sibling daemons: hft_backend + hft_backtest_launcher. These
    # don't have their own monitor; we surface their systemctl status
    # so a failed backend doesn't go unnoticed.
    for unit in [u.strip() for u in cfg["SIBLING_UNITS"].split(",") if u.strip()]:
        status = systemd_is_active(unit)
        if not status:
            continue  # systemctl unavailable; nothing to report
        if status in ("failed", "inactive"):
            key = f"sibling_{unit}"
            if state.can_alert(key, cooldown):
                notify(
                    notify_script,
                    f"{unit} is {status}",
                    "error",
                )
                state.mark_alerted(key)

    # Launcher heartbeat: state file should be touched at least once
    # per LAUNCHER_STATE_STALE_SEC. If the file is older OR missing
    # while the unit is active, the loop is wedged.
    launcher_status = systemd_is_active("hft_backtest_launcher.service")
    if launcher_status == "active":
        age = launcher_state_age_sec(cfg["LAUNCHER_STATE_FILE"])
        stale = float(cfg["LAUNCHER_STATE_STALE_SEC"])
        if age is not None and age > stale:
            if state.can_alert("launcher_wedged", cooldown):
                notify(
                    notify_script,
                    f"hft_backtest_launcher state file is "
                    f"{age:.0f}s old (threshold {stale:.0f}s) -- "
                    f"loop may be wedged",
                    "error",
                )
                state.mark_alerted("launcher_wedged")


    # Engine up but blind. Only meaningful during the regular session:
    # outside it, MarketData is Down by design and alerting would be
    # noise. Requires the engine to actually be running -- a stopped
    # engine is the liveness check's business, not this one.
    blind_limit = float(cfg["MD_BLIND_ALERT_SEC"])
    if blind_limit > 0 and is_rth_now() and rss is not None:
        down_for = md_down_seconds(cfg["ENGINE_LOG"])
        if down_for is not None and down_for >= blind_limit:
            if state.can_alert("md_blind", cooldown):
                notify(
                    notify_script,
                    f"ENGINE BLIND: running during RTH but market data has "
                    f"been Down for {down_for / 60:.0f} min. Check for a "
                    f"competing IBKR login (10197) or a lapsed market-data "
                    f"subscription (10089).",
                    "error",
                )
                state.mark_alerted("md_blind")

    # Deployed but never restarted. Only meaningful while the engine is
    # up; a stopped engine will pick up the new binary when it starts,
    # which is the normal deploy path rather than a fault.
    engine_bin = cfg.get("ENGINE_BIN", "")
    if engine_bin and rss is not None:
        pid = hft_app_pid(cfg["HFT_APP_PATTERN"])
        if pid is not None:
            mismatch = running_binary_mismatch(pid, engine_bin)
            if mismatch and state.can_alert("binary_mismatch", cooldown):
                notify(
                    notify_script,
                    f"BINARY MISMATCH: {mismatch}. The engine is still on the "
                    f"old build; restart it to pick up the deploy.",
                    "error",
                )
                state.mark_alerted("binary_mismatch")


def main(argv: list[str]) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--once", action="store_true", help="Single tick then exit")
    p.add_argument("--config", type=Path, default=Path("/etc/hft/monitor.env"))
    args = p.parse_args(argv)

    cfg = load_config(args.config)
    state = State(Path(cfg["STATE_FILE"]))

    # Graceful shutdown on SIGTERM (systemd stop).
    stop = {"flag": False}

    def handle_term(signum, frame):
        stop["flag"] = True

    signal.signal(signal.SIGTERM, handle_term)
    signal.signal(signal.SIGINT, handle_term)

    poll_sec = float(cfg["POLL_SEC"])
    while not stop["flag"]:
        try:
            check_once(cfg, state)
        except Exception as exc:
            print(f"hft_monitor: tick error: {exc}", file=sys.stderr)
        if args.once:
            break
        # Sleep in small chunks so SIGTERM is responsive.
        slept = 0.0
        while not stop["flag"] and slept < poll_sec:
            time.sleep(0.5)
            slept += 0.5
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
