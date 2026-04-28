#!/usr/bin/env bash
# Local helper: runs the full test suite and a brief smoke test of hft_app
# to confirm logs/app.log is created with state transitions.
set -uo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-ucrt-ibkr"
LOG_FILE="${ROOT_DIR}/_state_logging_smoke.log"
export PATH=/ucrt64/bin:$PATH
{
  echo "==> Running hft_tests.exe"
  cd "${BUILD_DIR}"
  ./hft_tests.exe
  echo "tests exit=$?"
  echo "==> Smoke-running hft_app.exe (paper mode, capped runtime)"
  rm -rf logs
  # The app reads config.ini from CWD; copy a minimal sample if one exists.
  if [[ -f "${ROOT_DIR}/config.ini" ]]; then
    cp "${ROOT_DIR}/config.ini" .
  fi
  timeout 12 ./hft_app.exe || echo "hft_app exited (rc=$?)"
  echo "==> logs/app.log (if any):"
  if [[ -f logs/app.log ]]; then
    head -50 logs/app.log
  else
    echo "(no logs/app.log produced)"
  fi
} >"${LOG_FILE}" 2>&1
echo "smoke exit=$?"
echo "==> tail of ${LOG_FILE}:"
tail -120 "${LOG_FILE}"
