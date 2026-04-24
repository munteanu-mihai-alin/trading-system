# trading-system

A compact research and integration project for ranking, simulation, validation, and optional Interactive Brokers / TWS connectivity. The default build works without IBKR. The IBKR-enabled build uses the vendored TWS API under `third_party/twsapi/client` together with protobuf and the Intel decimal runtime.

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
  - only when `HFT_ENABLE_IBKR=ON`; builds the vendored TWS API as a dedicated static library

## Default local build

This is the simplest build and does not require IBKR dependencies.

```bash
cmake -S . -B build
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Expected outputs with a single-config generator such as Unix Makefiles or MinGW Makefiles:

- `build/hft_app` or `build/hft_app.exe`
- `build/hft_tests` or `build/hft_tests.exe`

## IBKR-enabled local build

The IBKR build expects:

- vendored TWS API at `third_party/twsapi/client`
- protobuf available through `CMAKE_PREFIX_PATH`
- Intel decimal runtime available through `CMAKE_PREFIX_PATH` or system library paths

Example:

```bash
cmake -S . -B build-ibkr -DHFT_ENABLE_IBKR=ON -DCMAKE_PREFIX_PATH="/path/to/deps/install"
cmake --build build-ibkr -j"$(nproc)"
```

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
  -DHFT_ENABLE_IBKR=ON \
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
  - default configure, build, and test path without special IBKR dependency setup
- `coverage`
  - runs the coverage helper script and uploads coverage artifacts
- `linux-deps-release`
  - builds the reusable Linux dependency bundle
  - intended to run only on pushes whose commit message contains `%REBUILD_DEPS`
- `ibkr-build`
  - restores the Linux dependency bundle from the `linux-deps` release asset, or rebuilds it if missing
  - configures the root project with `-DHFT_ENABLE_IBKR=ON`
  - points `CMAKE_PREFIX_PATH` at `dependencies/linux/install`

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

### Workflow generation helper

- `scripts/generate_ci_workflow.sh`
  - rewrites `.github/workflows/ci.yml` from a bash here-document
  - useful if you want the workflow file to be regenerated from one script source

## CMake notes

- Root entry point:
  - `CMakeLists.txt`
- Main option:
  - `HFT_ENABLE_IBKR`
- Default:
  - `OFF`
- IBKR-enabled builds expect:
  - vendored TWS API sources at `third_party/twsapi/client`
  - protobuf available as a CMake package
  - Intel decimal runtime library available to CMake/library search

## Runtime notes

- `hft_app` reads `config.ini` through a relative path
- if `config.ini` cannot be opened, startup diagnostics should report that and the app falls back to defaults
- if `mode=live`, the IBKR event loop may block before end-of-run summary output

## Repository expectations

For a healthy repo checkout, these locations should exist or be created by scripts:

- root build:
  - `build/`
- IBKR build:
  - `build-ibkr/` or `build-ucrt-ibkr/`
- vendored TWS API:
  - `third_party/twsapi/client`
- Linux dependency bundle output:
  - `dependencies/linux/install`
  - `dependencies/linux/linux-deps-ubuntu-latest.tar.gz`
- UCRT dependency output:
  - `dependencies/ucrt64/install`

## Notes

This repository is a research/integration codebase. The default build path is the simplest way to work on the project. Use the IBKR-specific paths only when you need the vendored TWS API and its dependency stack.
