#!/usr/bin/env bash
# Run hft_tests.exe directly (no ctest wrapper) 10 times to isolate
# whether the flaky segfault is in the test binary itself or in ctest.
set -uo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-ucrt-ibkr"

export PATH="/ucrt64/bin:${PATH}"
cd "${BUILD_DIR}"

passes=0
fails=0
for i in 1 2 3 4 5 6 7 8 9 10; do
  ./hft_tests.exe >/tmp/htt.log 2>&1
  rc=$?
  if [[ ${rc} -eq 0 ]]; then
    passes=$((passes+1))
    echo "run ${i} rc=${rc} OK"
  else
    fails=$((fails+1))
    last=$(tail -3 /tmp/htt.log | tr '\n' ' ')
    echo "run ${i} rc=${rc} FAIL last=${last}"
  fi
done
echo "---"
echo "passes=${passes} fails=${fails}"
