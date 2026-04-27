#!/usr/bin/env bash
set -euo pipefail

# Run from MSYS2 UCRT64 shell.
# This script first validates/downloads dependency source trees through
# stage_third_party_sources_ucrt.sh, then builds/install outputs into
# dependencies/ucrt64/.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THIRD_PARTY_DIR="${ROOT_DIR}/third_party"
DEPS_DIR="${ROOT_DIR}/dependencies/ucrt64"
BUILD_DIR="${DEPS_DIR}/build"
INSTALL_DIR="${DEPS_DIR}/install"

STAGE_SCRIPT="${ROOT_DIR}/scripts/stage_third_party_sources_ucrt.sh"

PROTOBUF_SRC="${THIRD_PARTY_DIR}/protobuf-29.3"
ABSEIL_SRC="${THIRD_PARTY_DIR}/abseil-cpp"
RDFP_SRC="${THIRD_PARTY_DIR}/IntelRDFPMathLib20U4"
IBKR_SRC="${THIRD_PARTY_DIR}/twsapi/client"

echo "==> Staging/checking third-party source trees"
chmod +x "${STAGE_SCRIPT}"
"${STAGE_SCRIPT}"

# Clean only dependency build/install outputs, never third_party sources.
rm -rf "${BUILD_DIR}" "${INSTALL_DIR}"

test -d "${PROTOBUF_SRC}" || { echo "Missing ${PROTOBUF_SRC}"; exit 1; }
test -d "${ABSEIL_SRC}" || { echo "Missing ${ABSEIL_SRC}"; exit 1; }
test -d "${RDFP_SRC}" || { echo "Missing ${RDFP_SRC}"; exit 1; }

if ! grep -q "define ABSL_NAMESPACE_BEGIN" "${ABSEIL_SRC}/absl/base/config.h"; then
  echo "ERROR: ${ABSEIL_SRC} is invalid: ABSL_NAMESPACE_BEGIN is not defined in absl/base/config.h"
  echo "       Try: FORCE_RESTAGE_DEPS=1 ./scripts/build_third_party_dependencies_ucrt.sh"
  exit 1
fi

if [[ ! -d "${IBKR_SRC}" ]]; then
  echo "WARNING: ${IBKR_SRC} is missing."
  echo "         Dependency libraries can still be built, but HFT_ENABLE_IBKR project builds will need TWS API sources."
fi

mkdir -p "${BUILD_DIR}" "${INSTALL_DIR}/bin" "${INSTALL_DIR}/lib" "${INSTALL_DIR}/include"

echo "==> Tool versions"
which gcc || true
gcc --version | head -n 1 || true
which g++ || true
g++ --version | head -n 1 || true
which cmake || true
cmake --version | head -n 1 || true

echo "==> Building Abseil from third_party/abseil-cpp"
rm -rf "${BUILD_DIR}/abseil"
cmake -S "${ABSEIL_SRC}" -B "${BUILD_DIR}/abseil" \
  -G "MinGW Makefiles" \
  -DCMAKE_C_COMPILER=gcc \
  -DCMAKE_CXX_COMPILER=g++ \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
  -DBUILD_SHARED_LIBS=ON \
  -DABSL_PROPAGATE_CXX_STD=ON
cmake --build "${BUILD_DIR}/abseil" -j"$(nproc)"
cmake --install "${BUILD_DIR}/abseil"

echo "==> Building protobuf 29.3 from third_party/protobuf-29.3 (static libs for MinGW stability)"
rm -rf "${BUILD_DIR}/protobuf"
cmake -S "${PROTOBUF_SRC}" -B "${BUILD_DIR}/protobuf" \
  -G "MinGW Makefiles" \
  -DCMAKE_C_COMPILER=gcc \
  -DCMAKE_CXX_COMPILER=g++ \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
  -DCMAKE_PREFIX_PATH="${INSTALL_DIR}" \
  -Dprotobuf_BUILD_TESTS=OFF \
  -Dprotobuf_BUILD_SHARED_LIBS=OFF \
  -Dprotobuf_ABSL_PROVIDER=package
cmake --build "${BUILD_DIR}/protobuf" -j"$(nproc)"
cmake --install "${BUILD_DIR}/protobuf"

echo "==> Building Intel decimal library from third_party/IntelRDFPMathLib20U4"

RDFP_SRC_ROOT=""
while IFS= read -r candidate; do
  if find "$candidate" -type f -name '*.c' | grep -q .; then
    RDFP_SRC_ROOT="$candidate"
    break
  fi
done < <(find "${RDFP_SRC}" -type f -name 'bid_functions.h' -printf '%h\n' 2>/dev/null | sort -u)

if [[ -z "${RDFP_SRC_ROOT}" ]]; then
  while IFS= read -r candidate; do
    if find "$candidate" -type f -name '*.c' | grep -q .; then
      RDFP_SRC_ROOT="$candidate"
      break
    fi
  done < <(find "${RDFP_SRC}" -type f -name 'bid_conf.h' -printf '%h\n' 2>/dev/null | sort -u)
fi

if [[ -z "${RDFP_SRC_ROOT}" ]]; then
  RDFP_SRC_ROOT="$(find "${RDFP_SRC}" -type f -name '*.c' -printf '%h\n' 2>/dev/null | sort | uniq -c | sort -nr | awk 'NR==1{print $2}')"
fi

if [[ -z "${RDFP_SRC_ROOT}" || ! -d "${RDFP_SRC_ROOT}" ]]; then
  echo "ERROR: Could not detect an Intel RDFP source root under ${RDFP_SRC}"
  echo "Diagnostics:"
  find "${RDFP_SRC}" -maxdepth 6 -type f \( -name 'bid_functions.h' -o -name 'bid_conf.h' -o -name '*.c' \) | sort || true
  exit 1
fi

echo "Using Intel RDFP source root: ${RDFP_SRC_ROOT}"

RDFP_SRC_ROOT_CMAKE="${RDFP_SRC_ROOT}"
if command -v cygpath >/dev/null 2>&1; then
  RDFP_SRC_ROOT_CMAKE="$(cygpath -m "${RDFP_SRC_ROOT}")"
fi

echo "Using Intel RDFP source root for CMake: ${RDFP_SRC_ROOT_CMAKE}"

if ! find "${RDFP_SRC_ROOT}" -type f -name '*.c' | grep -q .; then
  echo "ERROR: No .c source files found under ${RDFP_SRC_ROOT}"
  exit 1
fi

echo "Sample Intel RDFP C files:"
find "${RDFP_SRC_ROOT}" -type f -name '*.c' | sort | head -n 10 || true

RDFP_BUILD_SRC="${BUILD_DIR}/intelrdfp-srcwrap"
RDFP_BUILD_DIR="${BUILD_DIR}/intelrdfp"

rm -rf "${RDFP_BUILD_SRC}" "${RDFP_BUILD_DIR}"
mkdir -p "${RDFP_BUILD_SRC}" "${RDFP_BUILD_DIR}"

cat > "${RDFP_BUILD_SRC}/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.20)
project(intelrdfpmath_c_wrapper C)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(RDFP_SRC_ROOT "${RDFP_SRC_ROOT_CMAKE}")
file(TO_CMAKE_PATH "\${RDFP_SRC_ROOT}" RDFP_SRC_ROOT_NORM)
file(GLOB_RECURSE RDFP_SOURCES "\${RDFP_SRC_ROOT_NORM}/*.c")

# Exclude optional binary<->decimal conversion wrappers. TWS Decimal only needs
# BID decimal runtime/string conversion symbols such as __bid64_from_string and
# __bid64_to_string. bid_binarydecimal.c depends on optional binary128 wrapper
# configuration that does not compile cleanly in this MinGW/UCRT CMake wrapper.
list(FILTER RDFP_SOURCES EXCLUDE REGEX ".*[/\\]bid_binarydecimal\\.c$")

# The custom wrapper build exports the upstream Intel names like bid64_add,
# bid64_from_string, and bid64_to_string. IBKR/TWS Decimal.h expects libgcc-style
# names with a leading double underscore, e.g. __bid64_from_string.
#
# Add a tiny compatibility translation unit that forwards the TWS names to the
# Intel RDFP symbols built above. For binary64 conversions, avoid compiling the
# optional bid_binarydecimal.c unit and provide string-based compatibility
# conversions instead; this is sufficient for linking TWS Decimal.cpp and keeps
# the MinGW/UCRT build portable.
set(TWS_BID_COMPAT_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/tws_bid_compat.c")
file(WRITE "${TWS_BID_COMPAT_SOURCE}" [=[
#include <stdio.h>
#include <stdlib.h>

typedef unsigned long long BID_UINT64;

BID_UINT64 bid64_add(BID_UINT64, BID_UINT64, unsigned int, unsigned int*);
BID_UINT64 bid64_sub(BID_UINT64, BID_UINT64, unsigned int, unsigned int*);
BID_UINT64 bid64_mul(BID_UINT64, BID_UINT64, unsigned int, unsigned int*);
BID_UINT64 bid64_div(BID_UINT64, BID_UINT64, unsigned int, unsigned int*);
BID_UINT64 bid64_from_string(char*, unsigned int, unsigned int*);
void bid64_to_string(char*, BID_UINT64, unsigned int*);

BID_UINT64 __bid64_add(BID_UINT64 x, BID_UINT64 y, unsigned int rnd, unsigned int* flags) {
  return bid64_add(x, y, rnd, flags);
}

BID_UINT64 __bid64_sub(BID_UINT64 x, BID_UINT64 y, unsigned int rnd, unsigned int* flags) {
  return bid64_sub(x, y, rnd, flags);
}

BID_UINT64 __bid64_mul(BID_UINT64 x, BID_UINT64 y, unsigned int rnd, unsigned int* flags) {
  return bid64_mul(x, y, rnd, flags);
}

BID_UINT64 __bid64_div(BID_UINT64 x, BID_UINT64 y, unsigned int rnd, unsigned int* flags) {
  return bid64_div(x, y, rnd, flags);
}

BID_UINT64 __bid64_from_string(char* s, unsigned int rnd, unsigned int* flags) {
  return bid64_from_string(s, rnd, flags);
}

void __bid64_to_string(char* out, BID_UINT64 x, unsigned int* flags) {
  bid64_to_string(out, x, flags);
}

double __bid64_to_binary64(BID_UINT64 x, unsigned int rnd, unsigned int* flags) {
  (void)rnd;
  char buf[128];
  bid64_to_string(buf, x, flags);
  return strtod(buf, NULL);
}

BID_UINT64 __binary64_to_bid64(double x, unsigned int rnd, unsigned int* flags) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%.17g", x);
  return bid64_from_string(buf, rnd, flags);
}
]=])
list(APPEND RDFP_SOURCES "${TWS_BID_COMPAT_SOURCE}")
message(STATUS "RDFP_SRC_ROOT_NORM=\${RDFP_SRC_ROOT_NORM}")
list(LENGTH RDFP_SOURCES RDFP_SOURCES_COUNT)
message(STATUS "RDFP_SOURCES_COUNT=\${RDFP_SOURCES_COUNT}")
if(NOT RDFP_SOURCES)
  message(FATAL_ERROR "No .c sources found under detected Intel RDFP source root: \${RDFP_SRC_ROOT_NORM}")
endif()
add_library(intelrdfpmath STATIC \${RDFP_SOURCES})
target_include_directories(intelrdfpmath PUBLIC "\${RDFP_SRC_ROOT_NORM}")
target_compile_definitions(intelrdfpmath PRIVATE
  CALL_BY_REF=0
  GLOBAL_RND=0
  GLOBAL_FLAGS=0
  UNCHANGED_BINARY_FLAGS=0
)
set_target_properties(intelrdfpmath PROPERTIES OUTPUT_NAME "intelrdfpmath")
install(TARGETS intelrdfpmath
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)
install(DIRECTORY "\${RDFP_SRC_ROOT_NORM}/" DESTINATION include FILES_MATCHING PATTERN "bid*.h")
EOF

echo "==> Listing detected Intel RDFP C files"
echo "==> Excluding optional bid_binarydecimal.c from wrapper build"
find "${RDFP_SRC_ROOT}" -type f -name '*.c' | sort | head -n 50 || true

echo "==> Generated Intel RDFP wrapper CMakeLists.txt"
printf 'Generated wrapper path: %s\n' "${RDFP_BUILD_SRC}/CMakeLists.txt"
cat "${RDFP_BUILD_SRC}/CMakeLists.txt"

cmake -S "${RDFP_BUILD_SRC}" -B "${RDFP_BUILD_DIR}" \
  -G "MinGW Makefiles" \
  -DCMAKE_C_COMPILER=gcc \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}"

cmake --build "${RDFP_BUILD_DIR}" -j"$(nproc)"
cmake --install "${RDFP_BUILD_DIR}"

if [[ ! -f "${INSTALL_DIR}/lib/libintelrdfpmath.a" ]]; then
  echo "ERROR: Expected ${INSTALL_DIR}/lib/libintelrdfpmath.a was not produced."
  exit 1
fi

echo "==> Verifying Intel decimal symbols needed by TWS Decimal.cpp"
for sym in \
  __bid64_add \
  __bid64_sub \
  __bid64_mul \
  __bid64_div \
  __bid64_from_string \
  __bid64_to_string \
  __bid64_to_binary64 \
  __binary64_to_bid64
do
  if ! nm -g "${INSTALL_DIR}/lib/libintelrdfpmath.a" | grep -q "${sym}"; then
    echo "ERROR: ${INSTALL_DIR}/lib/libintelrdfpmath.a does not export ${sym}"
    echo "       The Intel RDFP wrapper build is incomplete for IBKR/TWS Decimal."
    echo "       Matching bid64 symbols found:"
    nm -g "${INSTALL_DIR}/lib/libintelrdfpmath.a" | grep -E '(__)?bid64|(__)?binary64' | head -n 120 || true
    exit 1
  fi
done

echo "==> Built Intel decimal library:"
find "${INSTALL_DIR}/lib" -maxdepth 1 -type f \( -name 'libintelrdfpmath.a' -o -name 'libintelrdfpmath.dll.a' -o -name 'intelrdfpmath.lib' \) | sort || true

echo "==> Leaving IBKR/TWS API in repo at third_party/twsapi/client"
echo "==> No IBKR/TWS staging or copying performed"

echo "Done."
echo "Installed dependency prefix: ${INSTALL_DIR}"
echo "-- include files --"
find "${INSTALL_DIR}/include" -maxdepth 1 -type f | sort || true
echo "-- library files --"
find "${INSTALL_DIR}/lib" -maxdepth 1 -type f | sort || true

cat <<EOF
Next step for your project:
  cmake -S . -B build-ucrt-ibkr \
    -G "MinGW Makefiles" \
    -DCMAKE_C_COMPILER=gcc \
    -DCMAKE_CXX_COMPILER=g++ \
    -DCMAKE_BUILD_TYPE=Release \
    -DHFT_ENABLE_IBKR=ON \
    -DCMAKE_PREFIX_PATH="${INSTALL_DIR}"
EOF
