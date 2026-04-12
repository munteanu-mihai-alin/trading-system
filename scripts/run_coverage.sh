#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-coverage"

rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

cmake -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="--coverage -O0 -g" \
      -DCMAKE_EXE_LINKER_FLAGS="--coverage" \
      ..
cmake --build . -j"$(nproc)"
ctest --output-on-failure

lcov --capture --directory . --output-file coverage.info \
     --ignore-errors mismatch,mismatch \
     --rc branch_coverage=1

lcov --remove coverage.info \
     '/usr/*' \
     '*/tests/*' \
     '*/build*/*' \
     --output-file coverage.filtered.info \
     --ignore-errors unused

genhtml coverage.filtered.info \
        --output-directory coverage-html \
        --branch-coverage \
        --legend

python3 "${ROOT_DIR}/scripts/coverage_summary.py" \
    --info coverage.filtered.info \
    --threshold 70 \
    --branch-threshold 50

echo "Coverage artifacts:"
echo "  ${BUILD_DIR}/coverage.filtered.info"
echo "  ${BUILD_DIR}/coverage-html/index.html"
