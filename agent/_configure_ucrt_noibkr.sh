#!/usr/bin/env bash
# Local helper: configure the project with HFT_ENABLE_IBKR=OFF using the
# UCRT64 toolchain. Mirrors the IBKR=ON configure but without TWS API.
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-ucrt-noibkr"

if [[ -x /ucrt64/bin/gcc.exe ]]; then
  UCRT64_GCC="$(cygpath -m /ucrt64/bin/gcc.exe)"
  UCRT64_GXX="$(cygpath -m /ucrt64/bin/g++.exe)"
  export CC="${UCRT64_GCC}"
  export CXX="${UCRT64_GXX}"
  export PATH="/ucrt64/bin:${PATH}"
fi

rm -rf "${BUILD_DIR}"
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
  -G "Unix Makefiles" \
  -DCMAKE_C_COMPILER="${CC:-gcc}" \
  -DCMAKE_CXX_COMPILER="${CXX:-g++}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DHFT_ENABLE_IBKR=OFF \
  -DCMAKE_PREFIX_PATH="${ROOT_DIR}/dependencies/ucrt64/install"
