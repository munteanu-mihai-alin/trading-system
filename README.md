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


## Intel decimal runtime detection

CMake now fails explicitly if the BID/intel decimal runtime cannot be found, instead of silently continuing to the final link step.
CI also prints the installed `libintelrdfpmath-dev` file list and linker cache matches to make Linux runner differences visible.


## Wider Linux decimal runtime search

CMake now searches common Linux library directories directly for the Intel decimal runtime, and CI prints the installed package file list plus linker cache entries before configuring the IBKR build.


## CI ordering fix for decimal runtime diagnostics

The protobuf/decimal diagnostics now run before the IBKR configure step, so missing decimal-runtime details are visible even when configuration fails early.


## Dedicated CI decimal-runtime inspection step

The IBKR CI job now includes a standalone `Inspect decimal runtime package` step before configuration.


## Linux BID archive detection

The CI decimal-runtime inspection now scans `/usr/lib/x86_64-linux-gnu/libbidgcc*.a` with `nm` to identify which archive exports `__bid64_to_string` and `__bid64_from_string`.
CMake also searches the `bidgcc` archive names explicitly.


## UCRT helper scripts

- `scripts/stage_third_party_sources_ucrt.sh` stages dependency source code into `third_party/`.
- `scripts/build_third_party_dependencies_ucrt.sh` builds from `third_party/` into `dependencies/ucrt64/`.


## MinGW/UCRT protobuf build fix

`scripts/build_third_party_dependencies_ucrt.sh` now cleans only `dependencies/ucrt64/build` and `dependencies/ucrt64/install`, never `third_party/`, and builds protobuf 29.3 as static libraries on MinGW/UCRT to avoid shared `libprotoc.dll` link failures.


## MinGW/UCRT BID runtime fix

The UCRT dependency build script no longer tries to build the Intel decimal source tree with `make`. Instead it copies headers and collects `libbidgcc*.a` archives from MSYS2 library directories into `dependencies/ucrt64/install/lib`.
The root `CMakeLists.txt` now also searches `/ucrt64/lib`, `/mingw64/lib`, and `${CMAKE_PREFIX_PATH}/lib` for BID archives.


## MSYS2 GCC runtime package check

`scripts/build_third_party_dependencies_ucrt.sh` now checks for `libbidgcc*.a` before the build, tries to install `mingw-w64-ucrt-x86_64-gcc-libs` and `mingw-w64-ucrt-x86_64-gcc` via `pacman` if they are missing, and gives a clearer failure message with a package-content check command if the archives still are not present.


## MinGW/UCRT Intel decimal build

`scripts/build_third_party_dependencies_ucrt.sh` now attempts to build the Intel Decimal Floating-Point Math Library from `third_party/IntelRDFPMathLib20U4/LIBRARY/src` using a generated CMake wrapper and installs `libintelrdfpmath.a` into `dependencies/ucrt64/install/lib`.
This is intended to give the root project a real MinGW/UCRT-built decimal library instead of relying on MSYS2-provided BID archives.


## Intel RDFP source directory detection

The UCRT dependency build script now detects the actual `LIBRARY/src` directory under `third_party/IntelRDFPMathLib20U4` and enumerates the `.c` source files explicitly before generating the MinGW CMake wrapper.


## Flexible Intel RDFP source detection

The UCRT dependency build script now detects the Intel RDFP source directory by searching for `bid_functions.h`, `bid_conf.h`, or the densest directory of `.c` files under `third_party/IntelRDFPMathLib20U4`, instead of assuming `LIBRARY/src` exists at a fixed path.


## Intel RDFP wrapper source glob fix

The generated MinGW CMake wrapper now uses `file(GLOB ...)` inside CMake against the detected Intel RDFP source directory, instead of embedding a shell-generated explicit source list.


## Intel RDFP recursive source build

The UCRT dependency build script now detects an Intel RDFP source root and uses `file(GLOB_RECURSE ...)` to compile `.c` files recursively under that root, instead of requiring source files directly in one directory.


## Intel RDFP path normalization for CMake

The UCRT dependency build script now converts the detected Intel RDFP source root to a Windows-style path with `cygpath -m` before generating the wrapper CMake file. This avoids CMake glob failures on `/d/...` style MSYS paths.


## Intel RDFP wrapper debug output

The UCRT dependency build script now prints the detected Intel RDFP `.c` files and the generated wrapper `CMakeLists.txt` before invoking CMake for the Intel decimal library build.


## Intel RDFP normalized wrapper variable

The generated Intel RDFP wrapper now embeds the normalized Windows-style source path in a `RDFP_SRC_ROOT` CMake variable, converts it with `file(TO_CMAKE_PATH ...)`, and reports `RDFP_SOURCES_COUNT` during configure.


## TWS API client layout detection

The project now detects whether `third_party/twsapi/client` already contains the IBKR client sources or whether they are nested one level deeper under `third_party/twsapi/client/client`.
The stage script also accepts either the `cppclient` root or the `client` directory directly.


## CMake nesting fix

The IBKR/TWS API block in `CMakeLists.txt` was rewritten as a clean balanced `if(HFT_ENABLE_IBKR) ... endif()` section to fix the malformed flow-control nesting.


## Removed stray duplicate TWS block

Deleted the broken duplicate IBKR/TWS snippet that remained between the test target section and the final `# TWS vendor linkage fix` block. That duplicate block contained the unmatched `endif()` causing the configure failure.


## Removed old IBKR block

Deleted the original IBKR block at lines 31–63 of `CMakeLists.txt`, leaving only the newer flexible TWS vendor linkage block.


## Protobuf linkage target fix

Replaced `protobuf::libprotobuf` in the newer TWS linkage block with `${PROTOBUF_LIBRARY}` and added `${PROTOBUF_INCLUDE_DIR}` to the relevant include directories, matching the existing protobuf detection style in this project.
