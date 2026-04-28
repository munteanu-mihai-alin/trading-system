#!/usr/bin/env bash
# Phase 2 verification helper: clean configure + build + ctest of the now-
# always-IBKR project against the UCRT64 dependency prefix. Captures full
# output and exits non-zero on any failure.
set -uo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG_FILE="${ROOT_DIR}/_phase2_build.log"
BUILD_DIR="${ROOT_DIR}/build-ucrt-ibkr"

rm -rf "${BUILD_DIR}"
{
  echo "==> configure"
  bash "${ROOT_DIR}/agent/_configure_ucrt.sh"
  rc=$?
  echo "==> configure rc=${rc}"
  if [[ ${rc} -ne 0 ]]; then exit ${rc}; fi

  echo "==> build"
  bash -c 'export PATH=/ucrt64/bin:$PATH; cmake --build "$1" -j"$(nproc)"' _ "${BUILD_DIR}"
  rc=$?
  echo "==> build rc=${rc}"
  if [[ ${rc} -ne 0 ]]; then exit ${rc}; fi

  echo "==> ctest"
  bash -c 'export PATH=/ucrt64/bin:$PATH; cd "$1" && ctest --output-on-failure' _ "${BUILD_DIR}"
  rc=$?
  echo "==> ctest rc=${rc}"
  exit ${rc}
} >"${LOG_FILE}" 2>&1
rc=$?
echo "phase2 verification exit=${rc}"
echo "==> tail of ${LOG_FILE}:"
tail -100 "${LOG_FILE}"
exit "${rc}"
