#!/usr/bin/env bash
# Local agent helper. Not version-controlled (see .gitignore).
# Runs hft_tests.exe from the UCRT64 build directory.
set -uo pipefail
export PATH="/ucrt64/bin:${PATH}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}/build-ucrt-ibkr"
./hft_tests.exe
echo "hft_tests exit code: $?"
