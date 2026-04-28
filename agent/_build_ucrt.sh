#!/usr/bin/env bash
# Local agent helper. Not version-controlled (see .gitignore).
# Configures and builds hft_app + hft_tests under build-ucrt-ibkr/.
set -euo pipefail
export PATH="/ucrt64/bin:${PATH}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
bash "${ROOT_DIR}/agent/_configure_ucrt.sh"
cmake --build "${ROOT_DIR}/build-ucrt-ibkr" -j"$(nproc)"
