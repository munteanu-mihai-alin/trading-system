#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(cd "${1:-${ROOT_DIR}/build}" && pwd)"
APP="${BUILD_DIR}/hft_app"

if [[ ! -x "${APP}" && -x "${APP}.exe" ]]; then
  APP="${APP}.exe"
fi

if [[ ! -x "${APP}" ]]; then
  echo "hft_app executable not found under ${BUILD_DIR}" >&2
  exit 1
fi

cd "${BUILD_DIR}"

rm -rf logs
rm -f config.ini shadow_results.csv

cat >config.ini <<'EOF'
[runtime]
top_k=0
steps=1

[broker]
mode=paper
host=127.0.0.1
paper_port=7497
live_port=7496
client_id=1
EOF

"${APP}" >hft_app_smoke.out 2>hft_app_smoke.err

test -s logs/app.log
grep -q "LoggingService started" logs/app.log
grep -q "APP LoadingConfig -> ConnectingBroker" logs/app.log
grep -q "APP ConnectingBroker -> Live" logs/app.log
grep -q "Broker Down -> Ready" logs/app.log
grep -q "APP Live -> ShuttingDown" logs/app.log
grep -q "mode=paper" hft_app_smoke.out
grep -q "Chronos engine stopped" hft_app_smoke.out

echo "hft_app paper/no-order smoke passed"
