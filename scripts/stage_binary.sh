#!/usr/bin/env bash
# Build the current git branch's hft_app and stage it as a versioned,
# branch-tagged binary under bin/versions/<version>/, alongside a
# binary.json manifest that scripts/backend/api.py's GET /binaries
# reads. That is what lets the mobile app offer branch selection and
# show only the config knobs that branch supports.
#
# Usage:
#   scripts/stage_binary.sh [version] [--desc "text"]
#
# version defaults to "<branch>-<shortsha>" (slashes in the branch name
# become dashes). Requires the Linux dependency bundle at
# dependencies/linux/install (same as CI); override with CMAKE_PREFIX_PATH
# or the build dir with BUILD_DIR.
#
# This is the interim, manual path. The intended long-term source of
# per-branch binaries is CI artifacts / GitHub Packages.

set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

BRANCH="$(git rev-parse --abbrev-ref HEAD)"
SHORTSHA="$(git rev-parse --short HEAD)"

VERSION=""
DESC=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --desc) DESC="$2"; shift 2 ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) VERSION="$1"; shift ;;
  esac
done
VERSION="${VERSION:-${BRANCH//\//-}-${SHORTSHA}}"
DESC="${DESC:-${BRANCH} @ ${SHORTSHA}}"

BUILD_DIR="${BUILD_DIR:-build}"
PREFIX="${CMAKE_PREFIX_PATH:-${ROOT_DIR}/dependencies/linux/install}"

echo "Staging binary: version=${VERSION} branch=${BRANCH} commit=${SHORTSHA}"

# Configure once; cmake --build auto-reconfigures if CMakeLists changed.
if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
  echo "Configuring ${BUILD_DIR} (prefix=${PREFIX}) ..."
  cmake -S . -B "${BUILD_DIR}" -DCMAKE_PREFIX_PATH="${PREFIX}"
fi

echo "Building hft_app ..."
cmake --build "${BUILD_DIR}" --target hft_app -j"$(nproc)"

if [[ ! -x "${BUILD_DIR}/hft_app" ]]; then
  echo "ERROR: ${BUILD_DIR}/hft_app not produced" >&2
  exit 1
fi

DEST="bin/versions/${VERSION}"
mkdir -p "${DEST}"
cp "${BUILD_DIR}/hft_app" "${DEST}/hft_app"
chmod +x "${DEST}/hft_app"

# Manifest consumed by GET /binaries (_list_binaries in api.py).
cat > "${DEST}/binary.json" <<EOF
{
  "version": "${VERSION}",
  "branch": "${BRANCH}",
  "commit": "${SHORTSHA}",
  "built_at": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "description": "${DESC}"
}
EOF

echo "Staged ${DEST}/hft_app"
echo "--- manifest ---"
cat "${DEST}/binary.json"
