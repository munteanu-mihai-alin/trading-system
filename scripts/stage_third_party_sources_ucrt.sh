#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THIRD_PARTY_DIR="${ROOT_DIR}/third_party"

echo "==> Using existing TWS API in repo"
echo "    Expected path: ${THIRD_PARTY_DIR}/twsapi/client"

if [[ ! -d "${THIRD_PARTY_DIR}/twsapi/client" ]]; then
  echo "ERROR: ${THIRD_PARTY_DIR}/twsapi/client does not exist."
  echo "Place the IBKR TWS API client sources there before building."
  exit 1
fi

if [[ -d "${THIRD_PARTY_DIR}/twsapi/client/client" ]]; then
  echo "WARNING: nested path detected: ${THIRD_PARTY_DIR}/twsapi/client/client"
  echo "This usually means the API was copied twice."
  echo "Expected layout is third_party/twsapi/client/*.cpp"
fi

echo "==> No TWS API staging performed"
echo "Done."
