#!/usr/bin/env bash
# Show what's running on Hetzner + the local L1 backfill state.
# Quick situational-awareness check without paying ssh latency for every
# field.
#
# Usage:
#   scripts/hft_status.sh                # one-shot snapshot
#   scripts/hft_status.sh --kill         # ALSO kill any running hft_app
#                                          + orphan downloader subprocesses
#   scripts/hft_status.sh --tail         # also tail the latest run log
#
# Output sections:
#   - Hetzner: hft_app PID(s), latest log file + last 5 non-HEALTH lines,
#     reports/orders.csv tail (most recent placed/filled events),
#     newest L2 cache entries, free RAM.
#   - Local: latest entries in logs/l1_backfill_*.log, count of files
#     in each data/l1/<window>/.

set -euo pipefail

REPO="${HFT_REPO:-/d/trading-system}"
HETZNER_DIR="/mnt/HC_Volume_105581071/trading-system"
cd "$REPO"

DO_KILL=0
DO_TAIL=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --kill) DO_KILL=1; shift ;;
    --tail) DO_TAIL=1; shift ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 1 ;;
  esac
done

echo "===== Hetzner ====="
if [[ "$DO_KILL" == "1" ]]; then
  echo "+ killing hft_app + downloaders"
  ssh -o BatchMode=yes hetzner "pgrep -af 'bin/hft_app|databento_download_l2|local_l1_csv_provider' | grep -v pgrep | awk '{print \$1}' | xargs -r kill 2>/dev/null; sleep 2; pgrep -af 'bin/hft_app|databento_download_l2|local_l1_csv_provider' | grep -v pgrep | awk '{print \$1}' | xargs -r kill -9 2>/dev/null; true"
fi

ssh -o BatchMode=yes hetzner "
cd $HETZNER_DIR
echo '--- processes ---'
pgrep -af 'bin/hft_app|databento_download|local_l1_csv' | grep -v pgrep || echo '  (none running)'
echo
echo '--- latest log ---'
LATEST=\$(ls -t logs/cpp_backtest_*.log 2>/dev/null | head -1)
if [ -n \"\$LATEST\" ]; then
  echo \"  \$LATEST\"
  grep -v HEALTH \"\$LATEST\" | tail -8
fi
echo
echo '--- reports/orders.csv tail ---'
tail -5 reports/orders.csv 2>/dev/null || echo '  (none)'
echo
echo '--- newest L2 caches (top 3 by mtime) ---'
ls -lt data/l2/*/*.mbp10.csv 2>/dev/null | head -3 || echo '  (none)'
echo
echo '--- free RAM ---'
free -h | head -2
"

if [[ "$DO_TAIL" == "1" ]]; then
  echo
  echo "===== full latest log tail (non-HEALTH) ====="
  ssh -o BatchMode=yes hetzner "cd $HETZNER_DIR && LATEST=\$(ls -t logs/cpp_backtest_*.log | head -1); grep -v HEALTH \"\$LATEST\" | tail -50"
fi

echo
echo "===== Local ====="
echo "--- L1 backfill logs ---"
ls -t logs/l1_backfill_*.log 2>/dev/null | head -3 | while read f; do
  echo "  $f"
  tail -2 "$f" 2>/dev/null | sed 's/^/    /'
done
echo
echo "--- data/l1 windows ---"
for d in data/l1/*/; do
  [ -d "$d" ] || continue
  COUNT=$(ls "$d" 2>/dev/null | wc -l | tr -d ' ')
  echo "  $d ($COUNT files)"
done 2>/dev/null
