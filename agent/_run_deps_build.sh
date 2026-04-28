#!/usr/bin/env bash
# Local agent helper. Not version-controlled (see .gitignore).
# Runs the UCRT64 dependency build with full logging captured to
# _build_deps.log so PowerShell does not intercept redirections.
set -uo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG_FILE="${ROOT_DIR}/_build_deps.log"
bash "${ROOT_DIR}/scripts/build_third_party_dependencies_ucrt.sh" >"${LOG_FILE}" 2>&1
rc=$?
echo "deps build exit=${rc}"
echo "==> tail of ${LOG_FILE}:"
tail -100 "${LOG_FILE}"
exit "${rc}"
