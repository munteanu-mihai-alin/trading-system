#!/usr/bin/env bash
# Install / refresh the HFT engine's systemd units on Hetzner.
#
# Run as root from the repo root:
#   sudo bash scripts/systemd/install.sh
#
# What this does:
#   1. Copies the .service / .timer files into /etc/systemd/system/
#   2. Copies the logrotate config into /etc/logrotate.d/
#   3. Creates /etc/hft/ (the notify.env + monitor.env home)
#   4. Reloads systemd, enables the units, prints the operator commands
#
# Idempotent: re-running just refreshes the files. Existing
# /etc/hft/notify.env + /etc/hft/monitor.env are PRESERVED.

set -euo pipefail

if [[ $EUID -ne 0 ]]; then
  echo "install.sh: must run as root (try: sudo bash $0)" >&2
  exit 1
fi

REPO="${HFT_REPO:-/mnt/HC_Volume_105581071/trading-system}"
SYS=/etc/systemd/system
SCR=$REPO/scripts/systemd

if [[ ! -d "$SCR" ]]; then
  echo "install.sh: $SCR not found; clone the repo first" >&2
  exit 1
fi

echo "==> installing units from $SCR"
install -m 0644 "$SCR/hft_app.service"           "$SYS/hft_app.service"
install -m 0644 "$SCR/hft_app-rth-start.timer"   "$SYS/hft_app-rth-start.timer"
install -m 0644 "$SCR/hft_app-rth-stop.timer"    "$SYS/hft_app-rth-stop.timer"
install -m 0644 "$SCR/hft_app-rth-stop.service"  "$SYS/hft_app-rth-stop.service"
install -m 0644 "$SCR/hft-notify@.service"       "$SYS/hft-notify@.service"
install -m 0644 "$SCR/hft_monitor.service"       "$SYS/hft_monitor.service"
install -m 0644 "$SCR/hft_backend.service"       "$SYS/hft_backend.service"
install -m 0644 "$SCR/hft_backtest_launcher.service" \
                                                 "$SYS/hft_backtest_launcher.service"

echo "==> installing logrotate config"
install -m 0644 "$SCR/hft_app.logrotate" /etc/logrotate.d/hft_app

echo "==> ensuring /etc/hft exists"
mkdir -p /etc/hft
if [[ ! -f /etc/hft/notify.env ]]; then
  cat > /etc/hft/notify.env <<'EOF'
# Telegram (recommended): https://core.telegram.org/bots
#TG_BOT_TOKEN=
#TG_CHAT_ID=

# ntfy.sh phone push:
#NTFY_TOPIC=

# Both are optional. Without either, notify.sh logs to journald and
# exits 0 so systemd doesn't keep retrying the OnFailure hook.
EOF
  chmod 0600 /etc/hft/notify.env
  echo "  /etc/hft/notify.env created (edit to enable Telegram / ntfy)"
fi
if [[ ! -f /etc/hft/monitor.env ]]; then
  cat > /etc/hft/monitor.env <<'EOF'
# Thresholds for hft_monitor.py
VOLUME_PATH=/mnt/HC_Volume_105581071
DISK_FREE_ALERT_GB=20
MEM_FREE_ALERT_GB=1
RSS_ALERT_GB=12
POLL_SEC=30
ALERT_COOLDOWN_SEC=1800

# Set to true once paper trading is running so the monitor alerts
# when hft_app isn't found. Outside RTH leave this as false.
EXPECT_RUNNING=false
EOF
  chmod 0644 /etc/hft/monitor.env
  echo "  /etc/hft/monitor.env created (defaults are conservative)"
fi

echo "==> chmod helper scripts"
chmod +x "$REPO/scripts/notify.sh"
chmod +x "$REPO/scripts/hft_monitor.py"
chmod +x "$REPO/scripts/hft_backtest_launcher.py"
chmod +x "$REPO/scripts/backend/api.py"

echo "==> systemctl daemon-reload + enable"
systemctl daemon-reload
systemctl enable hft_app-rth-start.timer hft_app-rth-stop.timer \
                 hft_monitor.service \
                 hft_backend.service hft_backtest_launcher.service
systemctl start hft_app-rth-start.timer hft_app-rth-stop.timer \
                hft_monitor.service \
                hft_backend.service hft_backtest_launcher.service

echo "==> done. operator quick-reference:"
cat <<'EOF'

  systemctl status hft_app                  # current state
  systemctl start  hft_app                  # manual start (off-schedule)
  systemctl stop   hft_app                  # graceful stop
  journalctl -u hft_app -f                  # follow log
  systemctl list-timers --all               # RTH timers next-fire time

  kill -USR1 $(pgrep -f bin/hft_app)        # freeze trader
  kill -USR2 $(pgrep -f bin/hft_app)        # force-liquidate

  edit /etc/hft/notify.env  then systemctl restart hft_monitor.service
EOF
