#!/usr/bin/env bash
# L1 backfill driver - downloads top-of-book BID/ASK data for a fixed window
# into the dated-cache layout, then optionally scp's results to Hetzner.
#
# Two sources, picked via --source:
#   ibkr      (default) Uses ibkr_historical_l1.py against the local IB
#             Gateway. Free, but SPLIT-ADJUSTED retroactively, which
#             causes L1 vs L2 price disagreement for symbols that split
#             between the window date and "now" (see #todo in handoff
#             log). Fine for windows within ~3-6 months of now.
#   databento Uses databento_download_l1.py against the Databento
#             MBP-1 schema. Costs ~$0.20/symbol per ~10-day window
#             (full 49-symbol universe is $7-12 per window). Returns
#             raw exchange prices, consistent with the L2 MBP-10 cache.
#             Required for older windows (Yen 2024-08, COVID 2020-03)
#             where IBKR's adjusted prices diverge from real exchange
#             prints by 30-50%.
#
# Usage:
#   scripts/hft_l1_backfill.sh <window-name> [--source ibkr|databento]
#                              [--no-sync] [--start <date>] [--end <date>]
#                              [--symbols <file>] [--port <n>] [--client-id <n>]
#
# Window presets (override individual fields with --start/--end/--symbols):
#   yen      2024-08-02 -> 2024-08-09   config/symbols_yen.txt   (49 symbols)
#   covid    2020-03-09 -> 2020-03-20   config/symbols_covid.txt (45 symbols)
#   10day    2026-04-13 -> 2026-04-28   <none, default universe> (50 symbols)
#
# IBKR-source defaults:
#   - bar size: "1 min"
#   - port:     4002 (paper IB Gateway)
#   - client:   43 (separate from the trader's clientId=1)
#   - pacing:   inherited from ibkr_historical_l1.py (21s sleep, BID_ASK x2)
#   - wall:     ~5 min per symbol (49 sym = ~4h)
#
# Databento-source defaults:
#   - dataset:  XNAS.ITCH
#   - schema:   mbp-1
#   - wall:     ~10s per symbol (49 sym = ~10min) - parallel-ish on Hetzner
#   - cost:     see scripts/databento_l1_cost_quote.py for exact quote
#
# Output layout (matches include/broker/cache_filename.hpp):
#   data/l1/<startDate>_<endDate>/<SYMBOL>_<startISO>_<endISO>.mbp1.csv
#
# Log goes to logs/l1_backfill_<window-name>.log.

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
# IBKR uses YYYY-MM-DD with an EXCLUSIVE end (so end=Mon means data
# through Sun). Databento uses inclusive ISO timestamps.
# The filename (START_ISO / END_ISO) always reflects the actual covered
# window in basic ISO 8601 - this is what the broker's parse_filename
# expects (see include/broker/cache_filename.hpp).
case "$WINDOW" in
  yen)
    START="${START:-2024-08-02}"
    END="${END:-2024-08-10}"          # IBKR (exclusive at start-of-day)
    END_DB="2024-08-09T20:00:00Z"     # Databento (inclusive, market close)
    SYMBOLS_FILE="${SYMBOLS_FILE:-config/symbols_yen.txt}"
    START_ISO="20240802T133000Z"
    END_ISO="20240809T200000Z"
    FOLDER="2024-08-02_2024-08-09"
    ;;
  covid)
    START="${START:-2020-03-09}"
    END="${END:-2020-03-21}"          # IBKR exclusive
    END_DB="2020-03-20T20:00:00Z"
    SYMBOLS_FILE="${SYMBOLS_FILE:-config/symbols_covid.txt}"
    START_ISO="20200309T133000Z"
    END_ISO="20200320T200000Z"
    FOLDER="2020-03-09_2020-03-20"
    ;;
  10day)
    START="${START:-2026-04-13}"
    END="${END:-2026-04-29}"          # IBKR exclusive
    END_DB="2026-04-28T20:00:00Z"
    SYMBOLS_FILE="${SYMBOLS_FILE:-}"  # empty = default universe
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
SOURCE="ibkr"
DATABENTO_DATASET="XNAS.ITCH"
API_KEY_FILE="${DATABENTO_API_KEY_FILE:-$HOME/.config/trading-system/databento_api_key}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --source)     SOURCE="$2"; shift 2 ;;
    --dataset)    DATABENTO_DATASET="$2"; shift 2 ;;
    --api-key)    API_KEY_FILE="$2"; shift 2 ;;
    --no-sync)    DO_SYNC=0; shift ;;
    --start)      START="$2"; shift 2 ;;
    --end)        END="$2"; shift 2 ;;
    --symbols)    SYMBOLS_FILE="$2"; shift 2 ;;
    --port)       PORT="$2"; shift 2 ;;
    --client-id)  CLIENT_ID="$2"; shift 2 ;;
    -h|--help)    sed -n '2,50p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 1 ;;
  esac
done

if [[ "$SOURCE" != "ibkr" && "$SOURCE" != "databento" ]]; then
  echo "unknown --source: $SOURCE (try: ibkr, databento)" >&2
  exit 1
fi

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

echo "===== L1 backfill: $WINDOW (source=$SOURCE) ====="
echo "  window:    $START -> $END"
echo "  symbols:   $N_SYMBOLS (from ${SYMBOLS_FILE:-<default universe>})"
echo "  out dir:   $OUT_DIR"
echo "  log:       $LOG"
if [[ "$SOURCE" == "ibkr" ]]; then
  echo "  IB port:   $PORT  client-id: $CLIENT_ID"
else
  echo "  DB ds:     $DATABENTO_DATASET"
  echo "  DB key:    $API_KEY_FILE"
fi
echo "================================="

PYTHON="${HFT_PYTHON:-/d/trading-system/.venv-ibkr/Scripts/python.exe}"

# Source-specific preflight + downloader function selection.
if [[ "$SOURCE" == "ibkr" ]]; then
  # Verify IB Gateway is reachable before kicking off ~hours of pacing.
  if ! "$PYTHON" -c "
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
  download_one() {
    local sym="$1" out="$2"
    "$PYTHON" scripts/ibkr_historical_l1.py \
      --symbol "$sym" --start "$START" --end "$END" --output "$out" \
      --bar-size "1 min" --port "$PORT" --client-id "$CLIENT_ID" --timeout 60
  }
else
  # Databento preflight: confirm API key file is reachable.
  if [[ ! -f "$API_KEY_FILE" ]]; then
    echo "Databento API key file not found: $API_KEY_FILE" >&2
    echo "Set --api-key PATH or DATABENTO_API_KEY_FILE env var." >&2
    exit 1
  fi
  # Databento source runs ON Hetzner because:
  #  - the API key file lives there at /root/.config/trading-system/...
  #  - the databento python package is installed in its venv
  #  - the resulting file lands in Hetzner's data/l1/<window>/ where the
  #    backtest binary will read it (no local sync needed for backtest
  #    flow; pass --no-sync explicitly anyway since the file is already
  #    on the right machine).
  #
  # The local out path passed in is reused verbatim relative to the
  # Hetzner workdir.
  HETZNER_DIR_RUN="/mnt/HC_Volume_105581071/trading-system"
  HETZNER_KEY="/root/.config/trading-system/databento_api_key"
  download_one() {
    local sym="$1" out_rel="$2"
    ssh -o BatchMode=yes hetzner "cd $HETZNER_DIR_RUN && mkdir -p \$(dirname '$out_rel') && .venv/bin/python scripts/databento_download_l1.py --symbol '$sym' --start '${START}T13:30:00Z' --end '$END_DB' --output '$out_rel' --dataset '$DATABENTO_DATASET' --api-key-file '$HETZNER_KEY'"
  }
  # Local sync defaults to off for databento source: file's already on
  # the right machine. Honor --no-sync if set, otherwise still SCP back
  # at end (useful for local inspection).
fi

# Run the loop. Each symbol writes into the dated subdir with the
# canonical <SYMBOL>_<startISO>_<endISO>.mbp1.csv name so the broker's
# cross-folder glob will find it.
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
  if download_one "$SYM" "$OUT" >> "$LOG" 2>&1; then
    DONE=$((DONE + 1))
  else
    FAILED+=("$SYM")
    echo "  ! $SYM failed (see $LOG)" | tee -a "$LOG"
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
