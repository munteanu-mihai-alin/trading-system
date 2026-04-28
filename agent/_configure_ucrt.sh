#!/usr/bin/env bash
# Local agent helper. Not version-controlled (see .gitignore).
# Configures the project against the UCRT64 dependency prefix with
# HFT_ENABLE_IBKR=ON, using the MinGW-w64 UCRT64 gcc/g++ regardless of which
# /usr/bin/gcc the calling shell would otherwise pick up.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INSTALL_DIR="${ROOT_DIR}/dependencies/ucrt64/install"

if [[ -x /ucrt64/bin/gcc.exe ]]; then
  CC="$(cygpath -m /ucrt64/bin/gcc.exe)"
  CXX="$(cygpath -m /ucrt64/bin/g++.exe)"
  export PATH="/ucrt64/bin:${PATH}"
else
  CC=gcc
  CXX=g++
fi

cmake -S "${ROOT_DIR}" -B "${ROOT_DIR}/build-ucrt-ibkr" \
  -G "Unix Makefiles" \
  -DCMAKE_C_COMPILER="${CC}" \
  -DCMAKE_CXX_COMPILER="${CXX}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DHFT_ENABLE_IBKR=ON \
  -DCMAKE_PREFIX_PATH="${INSTALL_DIR}" \
  -DCMAKE_EXE_LINKER_FLAGS="-L${INSTALL_DIR}/lib" \
  -DCMAKE_SHARED_LINKER_FLAGS="-L${INSTALL_DIR}/lib"
