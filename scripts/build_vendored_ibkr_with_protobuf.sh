#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-ibkr"

rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

cmake -DHFT_ENABLE_IBKR=ON ..
cmake --build . -j"$(nproc)"

echo "Built IBKR-enabled target successfully in ${BUILD_DIR}"
