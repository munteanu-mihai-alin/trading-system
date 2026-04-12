#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-ibkr"

echo "This build expects protobuf 29.3 / C++ runtime 5.29.3 to match the vendored TWS API protobuf files."
echo "Install protobuf 29.3 first, then this script will configure and build the project."

rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

cmake -DHFT_ENABLE_IBKR=ON ..
cmake --build . -j"$(nproc)"

echo "Built IBKR-enabled target successfully in ${BUILD_DIR}"
