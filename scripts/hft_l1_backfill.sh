#!/usr/bin/env bash
# IBKR L1 backfill driver - downloads BID_ASK 1-min bars for a fixed window
# into the dated-cache layout, then optionally scp's the results to Hetzner.
#
# Usage:
#   scripts/hft_l1_backfill.sh <window-name> [--no-sync] [--start <date>]
#                              [--end <date>] [--symbols <file>]
#                              [--port <n>] [--client-id <n>]
#
# Window presets (override individual fields with --start/--end/--symbols):
#   yen      2024-08-02 -> 2024-08-09   config/symbols_yen.txt   (49 symbols)
#   covid    2020-03-09 -> 2020-03-20   config/symbols_covid.txt (45 symbols)
#   10day    2026-04-13 -> 2026-04-28   <none, default universe> (50 symbols)
#
# Defaults:
#   - bar size: "1 min"
#   - port:     4002 (paper IB Gateway)
#   - client:   43 (separate from the trader's clientId=1)
#   - timeout:  60s per request
#   - pacing:   inherited from ibkr_historical_l1.py (21s sleep, BID_ASK x2)
#
# Output layout (matches include/broker/cache_filename.hpp):
#   data/l1/<startDate>_<endDate>/<SYMBOL>_<startISO>_<endISO>.mbp1.csv
#
# Runs sequentially - IBKR rate-limits per account so parallel doesn't
# help. ~5 min wall per symbol; full 49-symbol Yen run is ~4h. Log goes
# to logs/l1_backfill_<window-name>.log.

set -euo pipefail

REPO="${HFT_REPO:-/d/trading-system}"
HETZNER_DIR="/mnt/HC_Volume_105581071/trading-system"
cd "$REPO"

WINDOW="${1:-}"
shift || true

if [[ -z "$WINDOW" ]]; then
  sed -n '2,30p' "$0"
  exit 1
fi

# Apply window preset.
case "$WINDOW" in
  yen)
    START="${START:-2024-08-02}"
    END="${END:-2024-08-10}"   # IBKR end is exclusive at start-of-day
    SYMBOLS_FILE="${SYMBOLS_FILE:-config/symbols_yen.txt}"
    # ISO-basic for filename time-range (must match the broker's cfg window
    # which uses 13:30:00Z open through 20:00:00Z close).
    START_ISO="20240802T133000Z"
    END_ISO="20240809T200000Z"
    FOLDER="2024-08-02_2024-08-09"
    ;;
  covid)
    START="${START:-2020-03-09}"
    END="${END:-2020-03-21}"   # exclusive end
    SYMBOLS_FILE="${SYMBOLS_FILE:-config/symbols_covid.txt}"
    START_ISO="20200309T133000Z"
    END_ISO="20200320T200000Z"
    FOLDER="2020-03-09_2020-03-20"
    ;;
  10day)
    START="${START:-2026-04-13}"
    END="${END:-2026-04-29}"   # exclusive end
    SYMBOLS_FILE="${SYMBOLS_FILE:-}"  # empty = parse from symbol_universe.hpp default
    START_ISO="20260413T133000Z"
    END_ISO="20260428T200000Z"
    FOLDER="2026-04-13_2026-04-28"
    ;;
  *)
    echo "unknown window preset: $WINDOW (try: yen, covid, 10day)" >&2
    exit 1
    ;;
esac

PORT=4002
CLIENT_ID=43
DO_SYNC=1

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-sync)    DO_SYNC=0; shift ;;
    --start)      START="$2"; shift 2 ;;
    --end)        END="$2"; shift 2 ;;
    --symbols)    SYMBOLS_FILE="$2"; shift 2 ;;
    --port)       PORT="$2"; shift 2 ;;
    --client-id)  CLIENT_ID="$2"; shift 2 ;;
    -h|--help)    sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 1 ;;
  esac
done

# Derive symbol list. If SYMBOLS_FILE is empty, the 10day preset uses the
# hard-coded 50-symbol universe; we fall back to the COVID list (which is
# a strict subset) since they share most of the universe.
if [[ -n "$SYMBOLS_FILE" && -f "$SYMBOLS_FILE" ]]; then
  SYMBOLS=$(grep -v '^#\|^$' "$SYMBOLS_FILE" | awk -F, '{print $1}' | tr '\n' ' ')
else
  echo "no --symbols file, defaulting to the 50-symbol universe baseline" >&2
  SYMBOLS="AAPL AMAT AMD AMKR APD ARM ASML ASX AWK CDNS CEG CSCO DD DELL ENTG GFS GSM HPE HPQ HWM IBM IMOS INTC KEYS KLAC LEA LIN LMT LRCX MKSI MU NIO NOC NOK NVDA OKLO PSTG QCOM RTX SMCI SNPS STX TSEM TSM TTE UMC VST WDC XPEV"
fi

N_SYMBOLS=$(echo "$SYMBOLS" | wc -w | tr -d ' ')
OUT_DIR="data/l1/$FOLDER"
LOG="logs/l1_backfill_${WINDOW}.log"

mkdir -p "$OUT_DIR" logs

echo "===== L1 backfill: $WINDOW ====="
echo "  window:    $START -> $END (IBKR --end is exclusive)"
echo "  symbols:   $N_SYMBOLS (from ${SYMBOLS_FILE:-<default universe>})"
echo "  out dir:   $OUT_DIR"
echo "  log:       $LOG"
echo "  port:      $PORT  client-id: $CLIENT_ID"
echo "================================="

# Verify IB Gateway is reachable before kicking off ~hours of pacing.
if ! "${HFT_PYTHON:-/d/trading-system/.venv-ibkr/Scripts/python.exe}" -c "
import socket
s = socket.socket(); s.settimeout(2)
try:
    s.connect(('127.0.0.1', $PORT))
    print('OK')
except Exception as e:
    raise SystemExit(f'IB Gateway not reachable on port $PORT: {e}')
finally:
    s.close()
"; then
  exit 1
fi

# Run the loop. Each symbol writes into the dated subdir with the
# canonical <SYMBOL>_<startISO>_<endISO>.mbp1.csv name so the broker's
# cross-folder glob will find it.
PYTHON="${HFT_PYTHON:-/d/trading-system/.venv-ibkr/Scripts/python.exe}"
DONE=0
SKIPPED=0
FAILED=()
for SYM in $SYMBOLS; do
  OUT="$OUT_DIR/${SYM}_${START_ISO}_${END_ISO}.mbp1.csv"
  if [[ -f "$OUT" && -s "$OUT" ]]; then
    echo "skip $SYM (exists: $OUT)"
    SKIPPED=$((SKIPPED + 1))
    continue
  fi
  echo "=== $SYM -> $OUT ===" | tee -a "$LOG"
  if "$PYTHON" scripts/ibkr_historical_l1.py \
       --symbol "$SYM" \
       --start "$START" --end "$END" \
       --output "$OUT" \
       --bar-size "1 min" \
       --port "$PORT" --client-id "$CLIENT_ID" --timeout 60 >> "$LOG" 2>&1; then
    DONE=$((DONE + 1))
  else
    FAILED+=("$SYM")
    echo "  ! $SYM failed (likely IBKR contract mismatch; see $LOG)" | tee -a "$LOG"
  fi
done

echo "===== L1 backfill done: $WINDOW ====="
echo "  symbols downloaded: $DONE"
echo "  symbols skipped:    $SKIPPED  (already present)"
if [[ "${#FAILED[@]}" -gt 0 ]]; then
  echo "  symbols failed:     ${#FAILED[@]} (${FAILED[*]})"
fi
echo "  total files in $OUT_DIR: $(ls "$OUT_DIR" 2>/dev/null | wc -l)"

# Optionally sync to Hetzner so a subsequent backtest can use them.
if [[ "$DO_SYNC" == "1" ]]; then
  echo "===== syncing to Hetzner ====="
  ssh -o BatchMode=yes hetzner "mkdir -p $HETZNER_DIR/data/l1/$FOLDER"
  scp -o BatchMode=yes "$OUT_DIR"/*.mbp1.csv "hetzner:$HETZNER_DIR/data/l1/$FOLDER/"
  echo "synced $N_SYMBOLS files to $HETZNER_DIR/data/l1/$FOLDER/"
fi

echo "done"
