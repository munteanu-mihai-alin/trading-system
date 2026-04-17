#!/usr/bin/env bash
set -euo pipefail

# Run from MSYS2 UCRT64 shell.
# This script expects dependency SOURCE CODE already staged in third_party/,
# and builds/install outputs into dependencies/ucrt64/.
#
# Expected trees:
#   third_party/protobuf-29.3
#   third_party/abseil-cpp
#   third_party/IntelRDFPMathLib20U4
#   third_party/twsapi/client

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THIRD_PARTY_DIR="${ROOT_DIR}/third_party"
DEPS_DIR="${ROOT_DIR}/dependencies/ucrt64"
BUILD_DIR="${DEPS_DIR}/build"
INSTALL_DIR="${DEPS_DIR}/install"

PROTOBUF_SRC="${THIRD_PARTY_DIR}/protobuf-29.3"
ABSEIL_SRC="${THIRD_PARTY_DIR}/abseil-cpp"
RDFP_SRC="${THIRD_PARTY_DIR}/IntelRDFPMathLib20U4"
IBKR_SRC="${THIRD_PARTY_DIR}/twsapi/client"

# Clean only dependency build/install outputs, never third_party sources.
rm -rf "${BUILD_DIR}" "${INSTALL_DIR}"

test -d "${PROTOBUF_SRC}" || { echo "Missing ${PROTOBUF_SRC}"; exit 1; }
test -d "${ABSEIL_SRC}" || { echo "Missing ${ABSEIL_SRC}"; exit 1; }
test -d "${RDFP_SRC}" || { echo "Missing ${RDFP_SRC}"; exit 1; }
test -d "${IBKR_SRC}" || { echo "Missing ${IBKR_SRC}"; exit 1; }

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
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
  -DCMAKE_PREFIX_PATH="${INSTALL_DIR}" \
  -Dprotobuf_BUILD_TESTS=OFF \
  -Dprotobuf_BUILD_SHARED_LIBS=OFF \
  -Dprotobuf_ABSL_PROVIDER=package
cmake --build "${BUILD_DIR}/protobuf" -j"$(nproc)"
cmake --install "${BUILD_DIR}/protobuf"


echo "==> Building Intel decimal library from third_party/IntelRDFPMathLib20U4"

# Detect the real Intel RDFP source root flexibly.
RDFP_SRC_ROOT=""
while IFS= read -r candidate; do
  if find "$candidate" -type f -name '*.c' | grep -q .; then
    RDFP_SRC_ROOT="$candidate"
    break
  fi
done < <(find "${RDFP_SRC}" -type f -name 'bid_functions.h' -printf '%h
' 2>/dev/null | sort -u)

if [[ -z "${RDFP_SRC_ROOT}" ]]; then
  while IFS= read -r candidate; do
    if find "$candidate" -type f -name '*.c' | grep -q .; then
      RDFP_SRC_ROOT="$candidate"
      break
    fi
  done < <(find "${RDFP_SRC}" -type f -name 'bid_conf.h' -printf '%h
' 2>/dev/null | sort -u)
fi

if [[ -z "${RDFP_SRC_ROOT}" ]]; then
  RDFP_SRC_ROOT="$(find "${RDFP_SRC}" -type f -name '*.c' -printf '%h
' 2>/dev/null | sort | uniq -c | sort -nr | awk 'NR==1{print $2}')"
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
find "${RDFP_SRC_ROOT}" -type f -name '*.c' | sort | head -n 50 || true

echo "==> Generated Intel RDFP wrapper CMakeLists.txt"
printf 'Generated wrapper path: %s
' "${RDFP_BUILD_SRC}/CMakeLists.txt"
cat "${RDFP_BUILD_SRC}/CMakeLists.txt"

cmake -S "${RDFP_BUILD_SRC}" -B "${RDFP_BUILD_DIR}" \
  -G "MinGW Makefiles" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}"

cmake --build "${RDFP_BUILD_DIR}" -j"$(nproc)"
cmake --install "${RDFP_BUILD_DIR}"

if [[ ! -f "${INSTALL_DIR}/lib/libintelrdfpmath.a" ]]; then
  echo "ERROR: Expected ${INSTALL_DIR}/lib/libintelrdfpmath.a was not produced."
  exit 1
fi

echo "==> Built Intel decimal library:"
find "${INSTALL_DIR}/lib" -maxdepth 1 -type f \( -name 'libintelrdfpmath.a' -o -name 'libintelrdfpmath.dll.a' -o -name 'intelrdfpmath.lib' \) | sort || true

echo "==> Copying IBKR headers/source snapshot into dependencies for local reference"
mkdir -p "${DEPS_DIR}/ibkr-client"
rm -rf "${DEPS_DIR}/ibkr-client/client"
cp -R "${IBKR_SRC}" "${DEPS_DIR}/ibkr-client/client"

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
    -DCMAKE_BUILD_TYPE=Release \
    -DHFT_ENABLE_IBKR=ON \
    -DCMAKE_PREFIX_PATH="${INSTALL_DIR}"
EOF


# Note:
# protobuf is built as static libraries on MinGW/UCRT because shared libprotoc/libprotobuf
# linking can fail on this toolchain with unresolved internal template symbols.


# Note:
# On MinGW/UCRT, the Intel decimal source tree is not built by this script.
# Instead, headers are copied from third_party/IntelRDFPMathLib20U4 and BID
# archives (libbidgcc*.a) are collected from MSYS2 library directories.


# Note:
# On MinGW/UCRT, this script now builds the Intel decimal library from
# third_party/IntelRDFPMathLib20U4/LIBRARY/src using a generated CMake wrapper
# and installs libintelrdfpmath.a into dependencies/ucrt64/install/lib.
