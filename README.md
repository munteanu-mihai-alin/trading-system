# trading-system

A compact research and integration project for ranking, simulation, validation, and Interactive Brokers / TWS connectivity. Every build links the vendored TWS API under `third_party/twsapi/client` together with protobuf and the Intel decimal runtime; broker-specific code is hidden behind the `IBKRTransport` seam (see `include/broker/IBKRTransport.hpp`) so non-broker code paths are easy to test in isolation.

## Project layout

Top-level structure:

- `include/`
  - public headers for models, execution, simulation, validation, engine, config, and broker abstractions
- `src/lib/`
  - reusable library code such as configuration loading, ranking, IBKR integration, and live execution wiring
- `src/app/`
  - application entry point (`hft_app`)
- `tests/`
  - unit, math, simulation, validation, and integration tests (`hft_tests`)
- `third_party/`
  - vendored or staged source trees
  - expected IBKR/TWS location: `third_party/twsapi/client`
- `scripts/`
  - local helper scripts for formatting, coverage, dependency staging, dependency builds, and CI bundle generation
- `.github/workflows/`
  - GitHub Actions workflows

## Build targets

Root `CMakeLists.txt` defines:

- `hft_lib`
  - main project library
- `hft_app`
  - application executable
- `hft_tests`
  - test executable
- `twsapi_vendor`
  - vendored TWS API compiled as a dedicated static library; always built
- `hft_gtests`
  - GoogleTest/GoogleMock-based test executable

## Local build

The build expects:

- vendored TWS API at `third_party/twsapi/client`
- protobuf available through `CMAKE_PREFIX_PATH`
- Intel decimal runtime available through `CMAKE_PREFIX_PATH` or system library paths
- `spdlog` and `GTest`/`GMock` available either through the system, through `CMAKE_PREFIX_PATH`, or through the vendored copies under `third_party/`

Example:

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/deps/install"
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Expected outputs with a single-config generator such as Unix Makefiles or MinGW Makefiles:

- `build/hft_app` or `build/hft_app.exe`
- `build/hft_tests` or `build/hft_tests.exe`
- `build/hft_gtests` or `build/hft_gtests.exe`

The app loads `config.ini` using a relative path, so run it from a working directory that contains the config file.

## MinGW / UCRT local build

The Windows/MSYS2 UCRT flow is split into two phases:

1. stage or validate source trees in `third_party/`
2. build third-party dependencies into `dependencies/ucrt64/install`

Stage/validate:

```bash
./scripts/stage_third_party_sources_ucrt.sh
```

Build dependencies:

```bash
./scripts/build_third_party_dependencies_ucrt.sh
```

Then configure the project:

```bash
cmake -S . -B build-ucrt-ibkr \
  -G "MinGW Makefiles" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="/path/to/repo/dependencies/ucrt64/install"
cmake --build build-ucrt-ibkr -j"$(nproc)"
```

### Expected UCRT locations

- staged source trees:
  - `third_party/protobuf-29.3`
  - `third_party/abseil-cpp`
  - `third_party/IntelRDFPMathLib20U4`
  - `third_party/twsapi/client`
- dependency build tree:
  - `dependencies/ucrt64/build`
- installed UCRT dependency prefix:
  - `dependencies/ucrt64/install`
- installed headers:
  - `dependencies/ucrt64/install/include`
- installed libraries:
  - `dependencies/ucrt64/install/lib`

## Linux CI dependency bundle

CI can build a reusable Linux dependency bundle for the IBKR-enabled build.

Producer script:

```bash
./scripts/rebuild_linux_deps_ci.sh
```

This script produces:

- install prefix:
  - `dependencies/linux/install`
- archive:
  - `dependencies/linux/linux-deps-ubuntu-latest.tar.gz`

The script builds:

- Abseil from protobuf's submodule
- protobuf 29.3
- Intel decimal runtime bundle contents copied into the install prefix

### Linux CI release asset

The CI workflow can publish the archive as a release asset named:

- `linux-deps-ubuntu-latest.tar.gz`

under the GitHub release:

- `linux-deps`

The intended flow is:

- normal CI runs try to download the existing `linux-deps` asset
- if the asset does not exist yet, CI can rebuild and republish it
- an explicit refresh path is gated by commit messages containing `%REBUILD_DEPS`

## CI overview

Current workflow responsibilities are:

- `build-and-test`
  - restores the Linux dependency bundle from the `linux-deps` release asset (or rebuilds it if missing)
  - configures, builds, and tests the project against that bundle
- `coverage`
  - same dependency setup as `build-and-test`, plus a coverage build with `lcov` and threshold gating
- `linux-deps-release`
  - builds the reusable Linux dependency bundle
  - intended to run only on pushes whose commit message contains `%REBUILD_DEPS`

## Script reference

### Formatting and coverage

- `scripts/check_clang_format.sh`
  - formatting check used by CI
- `scripts/run_coverage_ci.sh`
  - coverage helper used by the coverage workflow job

### UCRT / Windows helper scripts

- `scripts/stage_third_party_sources_ucrt.sh`
  - prepares or validates required source trees under `third_party/`
  - should leave the vendored TWS API in `third_party/twsapi/client`
- `scripts/build_third_party_dependencies_ucrt.sh`
  - builds third-party dependencies from `third_party/`
  - installs them into `dependencies/ucrt64/install`

### Linux CI dependency script

- `scripts/rebuild_linux_deps_ci.sh`
  - builds the Linux CI dependency bundle into `dependencies/linux/install`
  - archives it as `dependencies/linux/linux-deps-ubuntu-latest.tar.gz`

## CMake notes

- Root entry point:
  - `CMakeLists.txt`
- Mandatory dependencies:
  - vendored TWS API sources at `third_party/twsapi/client`
  - protobuf available as a CMake package (`protobuf::libprotobuf`)
  - Intel decimal runtime library available to CMake/library search
  - `spdlog` (system, via prefix, or vendored under `third_party/spdlog`)
  - `GTest`/`GMock` (system, via prefix, or vendored under `third_party/googletest`)

## Runtime notes

- `hft_app` reads `config.ini` through a relative path
- if `config.ini` cannot be opened, startup diagnostics should report that and the app falls back to defaults
- if `mode=live`, the IBKR event loop may block before end-of-run summary output

## Repository expectations

For a healthy repo checkout, these locations should exist or be created by scripts:

- root build:
  - `build/` (Linux/CI) or `build-ucrt-ibkr/` (Windows/MSYS2)
- vendored TWS API:
  - `third_party/twsapi/client`
- Linux dependency bundle output:
  - `dependencies/linux/install`
  - `dependencies/linux/linux-deps-ubuntu-latest.tar.gz`
- UCRT dependency output:
  - `dependencies/ucrt64/install`

## Notes

This repository is a research/integration codebase. Every build links the vendored TWS API stack; broker behaviour is mediated by the `IBKRTransport` interface so unit tests can substitute a mock without touching any TWS API headers.
