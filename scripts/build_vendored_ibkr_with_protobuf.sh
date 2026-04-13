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


# Recommended protobuf build settings for this project:
# cmake -S protobuf-29.3 -B protobuf-build \
#   -Dprotobuf_BUILD_TESTS=OFF \
#   -Dprotobuf_BUILD_SHARED_LIBS=ON \
#   -DCMAKE_BUILD_TYPE=Release \
#   -DCMAKE_INSTALL_PREFIX=/usr/local
