#!/usr/bin/env bash
# Local helper: clean reconfigure + build of hft_app/hft_tests against the
# UCRT64 dependency prefix, capturing full output.
set -uo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG_FILE="${ROOT_DIR}/_project_build.log"

rm -rf "${ROOT_DIR}/build-ucrt-ibkr"
{
  bash "${ROOT_DIR}/agent/_configure_ucrt.sh"
  echo "==> configure rc=$?"
  bash -c 'export PATH=/ucrt64/bin:$PATH; cmake --build "$1" -j"$(nproc)"' _ "${ROOT_DIR}/build-ucrt-ibkr"
  echo "==> build rc=$?"
} >"${LOG_FILE}" 2>&1
rc=$?
echo "project build exit=${rc}"
echo "==> tail of ${LOG_FILE}:"
tail -60 "${LOG_FILE}"
exit "${rc}"
