# HFT Merged Recovered

This project consolidates the previously generated code into a single modular codebase.

## Main components

- `include/models/`:
  Hawkes, OU, microstructure helpers, stock state, trade stats
- `include/execution/`:
  latency model, transaction cost model, slippage model, fill model, execution score
- `include/sim/`:
  FIFO order book, queue tracking, simulator
- `include/validation/`:
  calibration bins, KS statistic, rolling error, degradation alarm
- `include/engine/`:
  ranking engine interface and implementation
- `include/bench/`:
  RDTSC utilities and latency summary
- `tests/`:
  unit, math, simulator, and validation tests

## Build

```bash
mkdir build
cd build
cmake ..
make -j
ctest --output-on-failure
./hft_app
```

## Notes

This is a research platform, not a production trading system.


## Additive-only change policy

A guard script is included at:

```bash
scripts/enforce_additive_only.py
```

Usage:

```bash
python3 scripts/enforce_additive_only.py --base /path/to/old --candidate /path/to/new
```

It fails if:
- any file present in the base project is missing from the candidate project
- any text line from the base project was removed in the candidate project

This enforces "only add, do not remove" updates.

## IBKR SDK wiring

The project builds without the IBKR SDK by default.

To enable the real SDK:

```bash
cmake -DHFT_ENABLE_IBKR=ON -DHFT_IBKR_SDK_DIR=/path/to/IBKR ..
```

Expected SDK layout:

```text
<IBKR>/source/cppclient/client/
```

When enabled:
- `IBKRClient` compiles against the official IBKR C++ API
- live mode uses `IBKRClient`
- paper/sim mode uses `PaperBrokerSim`


## Vendored TWS API

The project now vendors the uploaded official IBKR C++ client under `third_party/twsapi/client`.
These files were copied verbatim from `TWS API/source/CppClient/client`.

To try a real IBKR build with the vendored SDK:

```bash
./scripts/build_vendored_ibkr.sh
```

Requirements for a real IBKR build:
- protobuf C++ development headers
- protobuf runtime library

If those dependencies are unavailable, the project still builds in stub mode with `HFT_ENABLE_IBKR=OFF`.


## Protobuf for vendored TWS API

The vendored TWS API C++ client requires protobuf development headers and library when `HFT_ENABLE_IBKR=ON`.

Ubuntu/Debian:

```bash
sudo apt-get install -y protobuf-compiler libprotobuf-dev
```

Build with:

```bash
./scripts/build_vendored_ibkr_with_protobuf.sh
```


## Exact protobuf version for this TWS API bundle

The vendored TWS API protobuf-generated headers require **protobuf 29.3** (C++ runtime 5.29.3). Older distro packages may be missing `google/protobuf/runtime_version.h`, and newer protobuf releases may fail the exact generated-code version check.


## Protobuf 29.3 installation note

In CI, protobuf 29.3 should be built with **CMake**, not `./configure`, for the release tarball used here.


## Protobuf 29.3 source retrieval note

For CI, protobuf 29.3 should be cloned with `--recurse-submodules` instead of using the release tarball, so the expected dependency tree is present during the CMake build.


## IBKR vendor linkage notes

The vendored TWS API is built as a dedicated `twsapi_vendor` static library and linked into `hft_lib`.
This avoids compiling vendor sources twice and makes protobuf/Abseil/BID dependency handling explicit.


## IBKR link dependencies on Linux

The IBKR-enabled build now expects two extra link dependencies in CI:
1. Matching Abseil, installed from the protobuf 29.3 source tree submodule.
2. Intel decimal floating-point math runtime, provided on Debian/Ubuntu by `libintelrdfpmath-dev`.


## Shared protobuf recommendation

For the IBKR-enabled Linux build, protobuf 29.3 should be built as a shared library (`-Dprotobuf_BUILD_SHARED_LIBS=ON`). This keeps the generated-code/runtime version matched while reducing static-link Abseil dependency issues.
