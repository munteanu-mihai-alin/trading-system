#!/usr/bin/env bash
# Run ctest 5 times in a row from a clean build directory to surface
# any flake we observed during Phase 2 verification.
set -uo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-ucrt-ibkr"
LOG_FILE="${ROOT_DIR}/_phase2_flake.log"

export PATH="/ucrt64/bin:${PATH}"
cd "${BUILD_DIR}"

passes=0
fails=0
{
  for i in 1 2 3 4 5 6 7 8 9 10; do
    out=$(ctest 2>&1)
    rc=$?
    summary=$(printf '%s' "${out}" | grep -E '(tests passed|tests failed|SegFault|Failed)' | tr '\n' ' ')
    echo "run ${i} rc=${rc} | ${summary}"
    if [[ ${rc} -eq 0 ]]; then
      passes=$((passes+1))
    else
      fails=$((fails+1))
    fi
  done
  echo "---"
  echo "passes=${passes} fails=${fails}"
} | tee "${LOG_FILE}"
