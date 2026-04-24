#!/usr/bin/env bash
set -euo pipefail

# Build Linux CI dependency bundle for the IBKR-enabled build.
# Produces:
#   dependencies/linux/install
#   dependencies/linux/linux-deps-ubuntu-latest.tar.gz
#
# Environment overrides:
#   PROTOBUF_TAG=v29.3
#   ROOT_DIR=/path/to/repo
#   DRY_RUN=1  # print planned work and exit before cloning/building

ROOT_DIR="${ROOT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
DEPS_DIR="${ROOT_DIR}/dependencies/linux"
BUILD_DIR="${DEPS_DIR}/build"
INSTALL_DIR="${DEPS_DIR}/install"
ARCHIVE_PATH="${DEPS_DIR}/linux-deps-ubuntu-latest.tar.gz"
THIRD_PARTY_DIR="${DEPS_DIR}/src"

PROTOBUF_TAG="${PROTOBUF_TAG:-v29.3}"
PROTOBUF_SRC="${THIRD_PARTY_DIR}/protobuf-29.3"
ABSEIL_SRC="${PROTOBUF_SRC}/third_party/abseil-cpp"

mkdir -p "${DEPS_DIR}" "${THIRD_PARTY_DIR}"

echo "==> Linux dependency bundle root: ${DEPS_DIR}"
echo "==> Install prefix: ${INSTALL_DIR}"
echo "==> Protobuf tag: ${PROTOBUF_TAG}"

if [[ "${DRY_RUN:-0}" == "1" ]]; then
  echo "DRY_RUN=1 -> would clone protobuf, build abseil+protobuf into ${INSTALL_DIR}, copy Intel decimal runtime, and archive ${ARCHIVE_PATH}"
  exit 0
fi

rm -rf "${BUILD_DIR}" "${INSTALL_DIR}" "${PROTOBUF_SRC}" "${ARCHIVE_PATH}"
mkdir -p "${BUILD_DIR}" "${INSTALL_DIR}" "${THIRD_PARTY_DIR}"

echo "==> Tool versions"
which cmake || true
cmake --version | head -n 1 || true
which g++ || true
g++ --version | head -n 1 || true
which git || true
git --version || true

echo "==> Cloning protobuf ${PROTOBUF_TAG} with submodules"
git clone --branch "${PROTOBUF_TAG}" --depth 1 --recurse-submodules \
  https://github.com/protocolbuffers/protobuf.git "${PROTOBUF_SRC}"

test -d "${ABSEIL_SRC}" || { echo "Missing ${ABSEIL_SRC}"; exit 1; }

echo "==> Building Abseil from protobuf submodule"
cmake -S "${ABSEIL_SRC}" -B "${BUILD_DIR}/abseil" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
  -DABSL_PROPAGATE_CXX_STD=ON \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON
cmake --build "${BUILD_DIR}/abseil" -j"$(nproc)"
cmake --install "${BUILD_DIR}/abseil"

echo "==> Building protobuf ${PROTOBUF_TAG} as shared libraries"
cmake -S "${PROTOBUF_SRC}" -B "${BUILD_DIR}/protobuf" \
  -Dprotobuf_BUILD_TESTS=OFF \
  -Dprotobuf_BUILD_SHARED_LIBS=ON \
  -Dprotobuf_ABSL_PROVIDER=package \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="${INSTALL_DIR}" \
  -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON
cmake --build "${BUILD_DIR}/protobuf" -j"$(nproc)"
cmake --install "${BUILD_DIR}/protobuf"

echo "==> Copying Intel decimal runtime into install prefix"
mkdir -p "${INSTALL_DIR}/include" "${INSTALL_DIR}/lib"

for hdr in /usr/include/bid_conf.h /usr/include/bid_functions.h; do
  if [[ -f "${hdr}" ]]; then
    cp -f "${hdr}" "${INSTALL_DIR}/include/"
  fi
done

copied_bid=0
for pattern in /usr/lib/x86_64-linux-gnu/libbidgcc*.a /usr/lib/x86_64-linux-gnu/libintelrdfpmath*; do
  shopt -s nullglob
  for lib in ${pattern}; do
    cp -f "${lib}" "${INSTALL_DIR}/lib/"
    copied_bid=1
  done
  shopt -u nullglob
done

if [[ ${copied_bid} -eq 0 ]]; then
  echo "ERROR: no Intel decimal runtime archives found under /usr/lib/x86_64-linux-gnu"
  exit 1
fi

echo "==> Installed dependency prefix contents"
find "${INSTALL_DIR}/include" -maxdepth 2 -type f | sort | head -n 50 || true
find "${INSTALL_DIR}/lib" -maxdepth 1 -type f | sort || true

echo "==> Creating archive ${ARCHIVE_PATH}"
tar -czf "${ARCHIVE_PATH}" -C "${ROOT_DIR}" dependencies/linux/install

echo "Done."
echo "Archive: ${ARCHIVE_PATH}"