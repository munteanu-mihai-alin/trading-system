#!/usr/bin/env bash
set -euo pipefail

# Run from MSYS2 UCRT64 shell.
# This script downloads dependency SOURCE CODE into third_party/.
#
# Downloads:
#   - protobuf 29.3 source tree
#   - matching abseil-cpp from protobuf submodule
#   - Intel Decimal Floating-Point Math Library source tree
#
# It also copies the locally MSI-installed IBKR API source into:
#   third_party/twsapi/client
#
# Usage example:
#   IBKR_API_DIR="/c/TWS API/source/cppclient" \
#   ./scripts/stage_third_party_sources_ucrt.sh
#
# Optional tools:
#   pacman -S --needed git curl unzip tar p7zip

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THIRD_PARTY_DIR="${ROOT_DIR}/third_party"
DOWNLOADS_DIR="${THIRD_PARTY_DIR}/_downloads"

PROTOBUF_TAG="${PROTOBUF_TAG:-v29.3}"
PROTOBUF_REPO_URL="https://github.com/protocolbuffers/protobuf.git"
INTEL_DEC_URL="${INTEL_DEC_URL:-https://www.netlib.org/misc/intel/IntelRDFPMathLib20U4.tar.gz}"
IBKR_API_DIR="${IBKR_API_DIR:-}"

if [[ -z "${IBKR_API_DIR}" ]]; then
  echo "Set IBKR_API_DIR first, for example:"
  echo '  IBKR_API_DIR="/c/TWS API/source/cppclient" ./scripts/stage_third_party_sources_ucrt.sh'
  exit 1
fi

test -d "${IBKR_API_DIR}" || { echo "Missing IBKR_API_DIR=${IBKR_API_DIR}"; exit 1; }

mkdir -p "${THIRD_PARTY_DIR}" "${DOWNLOADS_DIR}"

echo "==> Downloading protobuf ${PROTOBUF_TAG} with submodules"
rm -rf "${THIRD_PARTY_DIR}/protobuf-29.3"
git clone --branch "${PROTOBUF_TAG}" --depth 1 --recurse-submodules \
  "${PROTOBUF_REPO_URL}" "${THIRD_PARTY_DIR}/protobuf-29.3"

if [[ -d "${THIRD_PARTY_DIR}/protobuf-29.3/third_party/abseil-cpp" ]]; then
  echo "==> Copying matching Abseil source from protobuf submodule"
  rm -rf "${THIRD_PARTY_DIR}/abseil-cpp"
  cp -R "${THIRD_PARTY_DIR}/protobuf-29.3/third_party/abseil-cpp" "${THIRD_PARTY_DIR}/abseil-cpp"
else
  echo "ERROR: protobuf source does not contain third_party/abseil-cpp."
  echo "Make sure git submodules were fetched."
  exit 1
fi

echo "==> Downloading Intel decimal source archive"
rm -f "${DOWNLOADS_DIR}/IntelRDFPMathLib20U4.tar.gz"
curl -L --fail -o "${DOWNLOADS_DIR}/IntelRDFPMathLib20U4.tar.gz" "${INTEL_DEC_URL}"

echo "==> Extracting Intel decimal source into third_party/IntelRDFPMathLib20U4"
rm -rf "${THIRD_PARTY_DIR}/IntelRDFPMathLib20U4"
mkdir -p "${THIRD_PARTY_DIR}/IntelRDFPMathLib20U4"
if tar -xzf "${DOWNLOADS_DIR}/IntelRDFPMathLib20U4.tar.gz" -C "${THIRD_PARTY_DIR}/IntelRDFPMathLib20U4" --strip-components=1; then
  :
else
  rm -rf "${THIRD_PARTY_DIR}/IntelRDFPMathLib20U4"
  mkdir -p "${THIRD_PARTY_DIR}"
  tar -xzf "${DOWNLOADS_DIR}/IntelRDFPMathLib20U4.tar.gz" -C "${THIRD_PARTY_DIR}"
fi

echo "==> Staging local IBKR API source into third_party/twsapi/client"
mkdir -p "${THIRD_PARTY_DIR}/twsapi"
rm -rf "${THIRD_PARTY_DIR}/twsapi/client"

IBKR_CLIENT_DIR="${IBKR_API_DIR}"
if [[ -d "${IBKR_API_DIR}/client" ]]; then
  IBKR_CLIENT_DIR="${IBKR_API_DIR}/client"
fi

cp -R "${IBKR_CLIENT_DIR}" "${THIRD_PARTY_DIR}/twsapi/client"

echo "Done."
echo "Staged source trees:"
find "${THIRD_PARTY_DIR}" -maxdepth 2 -mindepth 1 -type d | sort
