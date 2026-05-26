#!/usr/bin/env bash
# End-to-end backtest driver: edit config -> launch on Hetzner -> wait ->
# archive -> sync back -> plot. All in one command.
#
# Usage:
#   scripts/hft_backtest.sh [--config <file>] [--target <pct>] [--label <name>]
#                           [--start <iso>] [--end <iso>] [--symbols <file>]
#                           [--no-wait] [--no-archive] [--no-sync] [--no-plot]
#                           [--kill-first] [--dry-run]
#
# Default flow:
#   1. SSH to Hetzner, apply config overrides in-place to config.ini
#   2. Clear stale flat reports/*.csv (prior run is assumed archived)
#   3. nohup launch bin/hft_app, log to logs/cpp_backtest_<ts>.log
#   4. Wait for the process to exit (polls every 30s, no token spend)
#   5. Archive flat artifacts into reports/runs/<run_id>/ (run_id = ISO
#      timestamp + run_label)
#   6. scp the run folder back to D:/trading-system/reports/runs/
#   7. Run scripts/plot_run.py against the synced folder; prints metrics.md
#
# Common patterns:
#   # Re-run last 10-day window with a tweaked target
#   scripts/hft_backtest.sh --target 0.012 --label v8_target012
#
#   # Run the COVID adversarial config
#   scripts/hft_backtest.sh --config config.databento_backtest.covid.ini
#
#   # Just launch + monitor, don't archive/sync yet
#   scripts/hft_backtest.sh --target 0.005 --no-archive --no-sync --no-plot
#
# Each flag is independent; e.g. you can re-archive a run later by calling
# scripts/hft_postmortem.sh <run_id>.

set -euo pipefail

# Shorthand prefix for the Hetzner workdir + standard library path.
HETZNER_DIR="/mnt/HC_Volume_105581071/trading-system"
LIB_PATH="\$PWD/dependencies/linux/install/lib"

# Defaults
CONFIG_FILE=""       # if non-empty, SCP this local file to Hetzner as config.ini
TARGET=""            # if non-empty, override target_profit_pct
LABEL=""             # if non-empty, override run_label
START=""             # if non-empty, override databento_start
END=""               # if non-empty, override databento_end
SYMBOLS=""           # if non-empty, override symbol_universe_path
DO_WAIT=1
DO_ARCHIVE=1
DO_SYNC=1
DO_PLOT=1
KILL_FIRST=0
DRY_RUN=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --config)      CONFIG_FILE="$2"; shift 2 ;;
    --target)      TARGET="$2"; shift 2 ;;
    --label)       LABEL="$2"; shift 2 ;;
    --start)       START="$2"; shift 2 ;;
    --end)         END="$2"; shift 2 ;;
    --symbols)     SYMBOLS="$2"; shift 2 ;;
    --no-wait)     DO_WAIT=0; shift ;;
    --no-archive)  DO_ARCHIVE=0; shift ;;
    --no-sync)     DO_SYNC=0; shift ;;
    --no-plot)     DO_PLOT=0; shift ;;
    --kill-first)  KILL_FIRST=1; shift ;;
    --dry-run)     DRY_RUN=1; shift ;;
    -h|--help)     sed -n '2,40p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 1 ;;
  esac
done

# We assume the local repo is at D:/trading-system (Windows + MSYS2). All
# the paths below are relative to that.
REPO="${HFT_REPO:-/d/trading-system}"
cd "$REPO"

# ----- helpers -----------------------------------------------------------

run() {
  # Echoes the command then runs it. With --dry-run, just echoes.
  echo "+ $*"
  if [[ "$DRY_RUN" == "0" ]]; then
    "$@"
  fi
}

ssh_run() {
  # Runs the given command on Hetzner inside the workdir. Single string,
  # quoting is the caller's responsibility.
  run ssh -o BatchMode=yes hetzner "cd $HETZNER_DIR && $1"
}

ssh_capture() {
  # Like ssh_run but captures stdout (skips dry-run noise).
  if [[ "$DRY_RUN" == "1" ]]; then
    echo "+ ssh hetzner cd $HETZNER_DIR && $1" >&2
    echo ""
  else
    ssh -o BatchMode=yes hetzner "cd $HETZNER_DIR && $1"
  fi
}

# ----- stage 0: optional kill of any running backtest --------------------

if [[ "$KILL_FIRST" == "1" ]]; then
  ssh_run "pgrep -af 'bin/hft_app|databento_download|local_l1_csv' | grep -v pgrep | awk '{print \$1}' | xargs -r kill 2>/dev/null; sleep 2; true"
fi

# ----- stage 1: stage a config file if requested -------------------------

if [[ -n "$CONFIG_FILE" ]]; then
  if [[ ! -f "$CONFIG_FILE" ]]; then
    echo "config file not found: $CONFIG_FILE" >&2
    exit 1
  fi
  run scp -o BatchMode=yes "$CONFIG_FILE" "hetzner:$HETZNER_DIR/config.ini"
fi

# ----- stage 2: apply overrides via sed on the in-place config -----------

# Build sed expression list. Each is an in-place edit on config.ini.
SED_ARGS=()
[[ -n "$TARGET" ]]  && SED_ARGS+=(-e "s|^target_profit_pct=.*|target_profit_pct=$TARGET|")
[[ -n "$LABEL" ]]   && SED_ARGS+=(-e "s|^run_label=.*|run_label=$LABEL|")
[[ -n "$START" ]]   && SED_ARGS+=(-e "s|^databento_start=.*|databento_start=$START|")
[[ -n "$END" ]]     && SED_ARGS+=(-e "s|^databento_end=.*|databento_end=$END|")
[[ -n "$SYMBOLS" ]] && SED_ARGS+=(-e "s|^symbol_universe_path=.*|symbol_universe_path=$SYMBOLS|")

if [[ "${#SED_ARGS[@]}" -gt 0 ]]; then
  # Quote each sed expression for the remote shell.
  REMOTE_SED=""
  for a in "${SED_ARGS[@]}"; do
    if [[ "$a" == "-e" ]]; then
      REMOTE_SED+=" -e"
    else
      REMOTE_SED+=" '$a'"
    fi
  done
  ssh_run "sed -i$REMOTE_SED config.ini"
fi

# Read back the active config so we know the run_label and window for later.
ACTIVE_CONFIG="$(ssh_capture "grep -E '^(target_profit_pct|run_label|databento_start|databento_end|symbol_universe_path)=' config.ini")"
echo "----- active config -----"
echo "$ACTIVE_CONFIG"
echo "-------------------------"

ACTIVE_LABEL=$(echo "$ACTIVE_CONFIG" | awk -F= '$1=="run_label"{print $2}')
if [[ -z "$ACTIVE_LABEL" ]]; then
  ACTIVE_LABEL="cpp_backtest"
fi

# ----- stage 3: clear stale flat reports + launch ------------------------

ssh_run "rm -f reports/decisions.csv reports/orders.csv reports/step_trace.csv reports/l2_trace.csv"

# Capture the timestamped log path; the launched binary writes there.
LAUNCH_RESULT="$(ssh_capture "LOG=logs/cpp_backtest_\$(date +%Y%m%dT%H%M%S).log; nohup env LD_LIBRARY_PATH=$LIB_PATH bin/hft_app > \$LOG 2>&1 & disown; sleep 2; echo \"PID=\$(pgrep -fx '.*/bin/hft_app$' | head -1)\"; echo \"LOG=\$LOG\"")"
echo "$LAUNCH_RESULT"

# ----- stage 4: optionally wait for completion ---------------------------

if [[ "$DO_WAIT" == "1" ]]; then
  echo "waiting for hft_app to finish (polls every 30s)..."
  if [[ "$DRY_RUN" == "0" ]]; then
    while ssh -o BatchMode=yes hetzner "pgrep -af 'bin/hft_app' | grep -v pgrep > /dev/null"; do
      sleep 30
    done
  fi
  echo "hft_app exited"
fi

# ----- stage 5: archive into reports/runs/<run_id>/ ----------------------

if [[ "$DO_ARCHIVE" == "1" ]]; then
  # Build run_id as ISO-minute + label, matching prior runs' folder names.
  RUN_ID="$(date -u +%Y-%m-%dT%H%M)_${ACTIVE_LABEL}"
  ssh_run "mkdir -p reports/runs/$RUN_ID && cp -p config.ini reports/runs/$RUN_ID/ && cp -p reports/decisions.csv reports/orders.csv reports/step_trace.csv reports/l2_trace.csv reports/runs/$RUN_ID/ 2>/dev/null; LATEST_LOG=\$(ls -t logs/cpp_backtest_*.log | head -1); cp -p \$LATEST_LOG reports/runs/$RUN_ID/stdout.log; ls -la reports/runs/$RUN_ID/"
  echo "RUN_ID=$RUN_ID"
fi

# ----- stage 6: sync back locally ----------------------------------------

if [[ "$DO_SYNC" == "1" ]] && [[ "$DO_ARCHIVE" == "1" ]]; then
  mkdir -p "$REPO/reports/runs"
  run scp -o BatchMode=yes -r "hetzner:$HETZNER_DIR/reports/runs/$RUN_ID" "$REPO/reports/runs/"
fi

# ----- stage 7: plot + metrics -------------------------------------------

if [[ "$DO_PLOT" == "1" ]] && [[ "$DO_SYNC" == "1" ]] && [[ "$DO_ARCHIVE" == "1" ]]; then
  PYTHON="${HFT_PYTHON:-/d/trading-system/.venv-ibkr/Scripts/python.exe}"
  run "$PYTHON" scripts/plot_run.py "$REPO/reports/runs/$RUN_ID"
  echo "===== metrics.md ====="
  cat "$REPO/reports/runs/$RUN_ID/metrics.md" 2>/dev/null || true
  echo "======================"
fi

echo "done"
