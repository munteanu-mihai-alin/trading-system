#!/usr/bin/env bash
set -euo pipefail

# Run from MSYS2 UCRT64 shell.
# This script validates/downloads third-party source trees into third_party/.
#
# It downloads non-TWS dependencies into third_party/_downloads first, then
# moves/copies each finished source tree under third_party/.
#
# Expected final trees:
#   third_party/protobuf-29.3
#   third_party/abseil-cpp
#   third_party/IntelRDFPMathLib20U4
#   third_party/twsapi/client
#
# TWS API is not downloaded because IBKR distributes it separately. If missing,
# this script warns only.
#
# Environment overrides:
#   PROTOBUF_TAG=v29.3
#   PROTOBUF_REPO_URL=https://github.com/protocolbuffers/protobuf.git
#   INTEL_DEC_URL=https://www.netlib.org/misc/intel/IntelRDFPMathLib20U4.tar.gz
#   FORCE_RESTAGE_DEPS=1   # delete/re-download protobuf, abseil, Intel RDFP

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THIRD_PARTY_DIR="${ROOT_DIR}/third_party"
DOWNLOADS_DIR="${THIRD_PARTY_DIR}/_downloads"

PROTOBUF_TAG="${PROTOBUF_TAG:-v29.3}"
PROTOBUF_REPO_URL="${PROTOBUF_REPO_URL:-https://github.com/protocolbuffers/protobuf.git}"
INTEL_DEC_URL="${INTEL_DEC_URL:-https://www.netlib.org/misc/intel/IntelRDFPMathLib20U4.tar.gz}"
FORCE_RESTAGE_DEPS="${FORCE_RESTAGE_DEPS:-0}"

PROTOBUF_DIR="${THIRD_PARTY_DIR}/protobuf-29.3"
ABSEIL_DIR="${THIRD_PARTY_DIR}/abseil-cpp"
ABSEIL_FROM_PROTOBUF="${PROTOBUF_DIR}/third_party/abseil-cpp"
RDFP_DIR="${THIRD_PARTY_DIR}/IntelRDFPMathLib20U4"
TWSAPI_DIR="${THIRD_PARTY_DIR}/twsapi/client"

mkdir -p "${THIRD_PARTY_DIR}" "${DOWNLOADS_DIR}"

valid_abseil_dir() {
  local dir="$1"
  [[ -d "${dir}" ]] || return 1
  [[ -f "${dir}/CMakeLists.txt" ]] || return 1
  [[ -f "${dir}/absl/base/config.h" ]] || return 1
  [[ -f "${dir}/absl/base/options.h" ]] || return 1
  [[ -f "${dir}/absl/time/internal/cctz/src/time_zone_lookup.cc" ]] || return 1
  grep -q "define ABSL_NAMESPACE_BEGIN" "${dir}/absl/base/config.h" || return 1
  grep -q "define ABSL_NAMESPACE_END" "${dir}/absl/base/config.h" || return 1
}

has_protobuf() {
  [[ -f "${PROTOBUF_DIR}/src/google/protobuf/runtime_version.h" ]] &&
  valid_abseil_dir "${ABSEIL_FROM_PROTOBUF}"
}

has_abseil() {
  valid_abseil_dir "${ABSEIL_DIR}"
}

has_rdfp() {
  [[ -d "${RDFP_DIR}" ]] &&
  find "${RDFP_DIR}" -type f -name 'bid_functions.h' | grep -q . &&
  find "${RDFP_DIR}" -type f -name '*.c' | grep -q .
}

has_twsapi() {
  [[ -d "${TWSAPI_DIR}" ]] &&
  find "${TWSAPI_DIR}" -maxdepth 1 -type f \( -name 'EClient.cpp' -o -name 'EClient.h' \) | grep -q .
}

status_line() {
  local name="$1"
  local status="$2"
  local action="$3"
  printf '==> %-28s status=%-8s action=%s\n' "${name}" "${status}" "${action}"
}

download_protobuf() {
  rm -rf "${DOWNLOADS_DIR}/protobuf-29.3" "${PROTOBUF_DIR}"
  git clone --branch "${PROTOBUF_TAG}" --depth 1 --recurse-submodules \
    "${PROTOBUF_REPO_URL}" "${DOWNLOADS_DIR}/protobuf-29.3"
  if ! valid_abseil_dir "${DOWNLOADS_DIR}/protobuf-29.3/third_party/abseil-cpp"; then
    echo "ERROR: downloaded protobuf tree does not contain a valid Abseil submodule."
    echo "       Check git clone --recurse-submodules output and network/proxy issues."
    exit 1
  fi
  mv "${DOWNLOADS_DIR}/protobuf-29.3" "${PROTOBUF_DIR}"
}

stage_abseil_from_protobuf() {
  if ! valid_abseil_dir "${ABSEIL_FROM_PROTOBUF}"; then
    echo "ERROR: protobuf bundled Abseil is not valid at ${ABSEIL_FROM_PROTOBUF}"
    echo "       Re-downloading protobuf should have repaired this."
    exit 1
  fi

  rm -rf "${DOWNLOADS_DIR}/abseil-cpp" "${ABSEIL_DIR}"
  cp -R "${ABSEIL_FROM_PROTOBUF}" "${DOWNLOADS_DIR}/abseil-cpp"

  if ! valid_abseil_dir "${DOWNLOADS_DIR}/abseil-cpp"; then
    echo "ERROR: staged Abseil copy is invalid at ${DOWNLOADS_DIR}/abseil-cpp"
    exit 1
  fi

  mv "${DOWNLOADS_DIR}/abseil-cpp" "${ABSEIL_DIR}"

  if ! has_abseil; then
    echo "ERROR: Abseil is still invalid after staging at ${ABSEIL_DIR}"
    echo "Diagnostics:"
    ls -la "${ABSEIL_DIR}/absl/base" || true
    grep -n "ABSL_NAMESPACE_BEGIN" "${ABSEIL_DIR}/absl/base/config.h" || true
    exit 1
  fi
}

if [[ "${FORCE_RESTAGE_DEPS}" == "1" ]]; then
  echo "==> FORCE_RESTAGE_DEPS=1: removing non-TWS dependency source trees"
  rm -rf "${PROTOBUF_DIR}" "${ABSEIL_DIR}" "${RDFP_DIR}"
fi

echo "==> Checking third-party source trees under ${THIRD_PARTY_DIR}"

if has_protobuf; then
  status_line "protobuf-29.3" "present" "keep existing"
else
  status_line "protobuf-29.3" "missing" "download into third_party/_downloads then move to third_party/protobuf-29.3"
  download_protobuf
fi

if has_abseil; then
  status_line "abseil-cpp" "present" "keep existing"
else
  status_line "abseil-cpp" "missing" "copy matching Abseil from protobuf submodule"
  stage_abseil_from_protobuf
fi

if has_rdfp; then
  status_line "IntelRDFPMathLib20U4" "present" "keep existing"
else
  status_line "IntelRDFPMathLib20U4" "missing" "download archive into third_party/_downloads then extract/move"
  rm -rf "${DOWNLOADS_DIR}/IntelRDFPMathLib20U4" "${DOWNLOADS_DIR}/IntelRDFPMathLib20U4.extract" "${RDFP_DIR}"
  mkdir -p "${DOWNLOADS_DIR}/IntelRDFPMathLib20U4.extract"

  RDFP_ARCHIVE="${DOWNLOADS_DIR}/IntelRDFPMathLib20U4.tar.gz"
  curl -L --fail -o "${RDFP_ARCHIVE}" "${INTEL_DEC_URL}"

  tar -xzf "${RDFP_ARCHIVE}" -C "${DOWNLOADS_DIR}/IntelRDFPMathLib20U4.extract"

  extracted_root="${DOWNLOADS_DIR}/IntelRDFPMathLib20U4.extract"
  candidate_root=""
  if find "${extracted_root}" -type f -name 'bid_functions.h' | grep -q .; then
    candidate_root="$(find "${extracted_root}" -type f -name 'bid_functions.h' -printf '%h\n' | head -n 1)"
    while [[ "${candidate_root}" != "${extracted_root}" && ! -d "${candidate_root}/src" && "$(basename "${candidate_root}")" != "src" ]]; do
      candidate_root="$(dirname "${candidate_root}")"
    done
  fi

  if [[ -z "${candidate_root}" || ! -d "${candidate_root}" ]]; then
    echo "ERROR: Could not identify Intel RDFP source root inside ${RDFP_ARCHIVE}"
    find "${extracted_root}" -maxdepth 5 -type f | sort | head -n 100 || true
    exit 1
  fi

  mkdir -p "${DOWNLOADS_DIR}/IntelRDFPMathLib20U4"
  cp -R "${candidate_root}/." "${DOWNLOADS_DIR}/IntelRDFPMathLib20U4/"
  mv "${DOWNLOADS_DIR}/IntelRDFPMathLib20U4" "${RDFP_DIR}"
fi

if has_twsapi; then
  status_line "twsapi/client" "present" "keep existing"
else
  status_line "twsapi/client" "missing" "WARNING only; IBKR/TWS API is not downloaded by this script"
  echo "WARNING: missing ${TWSAPI_DIR}"
  echo "         Place the IBKR/TWS API client sources there before IBKR-enabled project builds."
fi

if [[ -d "${TWSAPI_DIR}/client" ]]; then
  echo "WARNING: nested TWS path detected: ${TWSAPI_DIR}/client"
  echo "         Expected layout is third_party/twsapi/client/*.cpp, not client/client."
fi

echo "==> Final third_party source status"
if has_protobuf; then echo "    protobuf-29.3: present"; else echo "    protobuf-29.3: missing"; fi
if has_abseil; then echo "    abseil-cpp: present"; else echo "    abseil-cpp: missing"; fi
if has_rdfp; then echo "    IntelRDFPMathLib20U4: present"; else echo "    IntelRDFPMathLib20U4: missing"; fi
if has_twsapi; then echo "    twsapi/client: present"; else echo "    twsapi/client: missing"; fi

if ! has_protobuf || ! has_abseil || ! has_rdfp; then
  echo "ERROR: required non-TWS dependencies are not valid after staging."
  exit 1
fi

echo "Done."
