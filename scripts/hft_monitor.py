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
    "NOTIFY_SCRIPT": "/mnt/HC_Volume_105581071/trading-system/scripts/notify.sh",
    "STATE_FILE": "/var/run/hft_monitor.state",
    "HFT_APP_PATTERN": "bin/hft_app",
    # When true, an absent hft_app process triggers an alert. Disable
    # while we're outside RTH or doing maintenance.
    "EXPECT_RUNNING": "false",
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
