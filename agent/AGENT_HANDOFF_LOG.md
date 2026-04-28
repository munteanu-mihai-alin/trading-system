# Agent Handoff Log

This is the append-only working log for agents. New entries should be added at the top.

Read `AGENT_WORKFLOW.md` before editing this file.

## [2026-04-28] - Phase 0: stage GoogleTest + GoogleMock everywhere, add hft_gtests smoke target

Model / agent:
- Claude Opus 4.7, reasoning model (Cursor agent)

Source state:
- Local working tree at `D:\trading-system` (git repo, branch `main`).
- Builds on top of the previous "drop HFT_ENABLE_STATE_LOGGING" handoff entry. Not yet pushed; CI for the parent commit `8aeaf39` was green.

User request:
- CI is failing because `Line coverage 59.88% is below threshold 70.00%`. Fix by splitting tests into UT (one real class with collaborators mocked) and MT (real objects, real wiring). UT means we need mocks. Use gtest+gmock for the new test infra. Stage gtest in the local UCRT scripts and the Linux CI runner.
- Don't remove tests; adapt the ones tied to IBKR API to use mocks. Keep the legacy framework alive so nothing breaks.
- This entry covers Phase 0 only (foundation: gtest available everywhere, plus a smoke `hft_gtests` target). Phase 1 (mockable seam in IBKRClient), Phase 2 (`tests/unit/` + `tests/module/` reorg keeping `tests/sim/`, `tests/math/`, `tests/validation/`, `tests/common/`), Phase 3 (UTs to push coverage > 70%), Phase 4 (cross-check IBGateway logs at `D:/ibgateway/eanaichgbgjlpppphnmlaclphamgjhbkajildhnp/gateway-exported-logs.txt`), and Phase 5 (deferred follow-ups: race-fix UT, drop Risk/Strategy from HEALTH, heartbeat on real IBKR activity, engine reaction to Broker Error, smoke in CI, LoggingService::Config) are queued for separate sessions.

What changed:

1. Vendored googletest at `third_party/googletest` (3.8 MB, 247 files)
   - Cloned `https://github.com/google/googletest.git` at tag `v1.15.2`, removed `.git`, `.github`, `ci/`, `docs/`, the bazel scaffolding (`BUILD.bazel`, `WORKSPACE*`, `MODULE.bazel`, `*.bzl`).
   - Kept the upstream top-level `CMakeLists.txt`, `googletest/`, `googlemock/`, `LICENSE`, `README.md`, `CONTRIBUTORS`, `CONTRIBUTING.md`.
   - Mirrors the spdlog vendoring approach.

2. `scripts/stage_third_party_sources_ucrt.sh`
   - New `GTEST_TAG` (default `v1.15.2`) + `GTEST_REPO_URL` overrides.
   - New `GTEST_DIR` and `has_gtest()` check (validates `CMakeLists.txt`, `googletest/include/gtest/gtest.h`, `googlemock/include/gmock/gmock.h`).
   - New `download_gtest()` that clones the pinned tag into `third_party/_downloads/googletest`, strips `.git`/`.github`, and moves into place.
   - `FORCE_RESTAGE_DEPS=1` now also wipes `third_party/googletest/`.
   - Final summary block reports googletest status; final hard-fail check requires googletest in addition to existing deps.

3. `scripts/build_third_party_dependencies_ucrt.sh`
   - New `GTEST_SRC` variable + `test -d` guard.
   - New cmake build step: `cmake -S third_party/googletest -B build/googletest` with `BUILD_GMOCK=ON`, `INSTALL_GTEST=ON`, `BUILD_SHARED_LIBS=OFF`, `gtest_force_shared_crt=ON`. Installs into `dependencies/ucrt64/install`.
   - Verifies the four expected static archives are produced: `libgtest.a`, `libgtest_main.a`, `libgmock.a`, `libgmock_main.a`.

4. `scripts/rebuild_linux_deps_ci.sh`
   - New `GTEST_TAG` (default `v1.15.2`) + clone step into `dependencies/linux/src/googletest`.
   - cmake build/install step: PIC-on, static, `BUILD_GMOCK=ON`, `INSTALL_GTEST=ON`. Installs into `dependencies/linux/install` (the same prefix that gets archived for the CI release asset).
   - `DRY_RUN=1` summary mentions googletest; cleanup removes `GTEST_SRC` alongside the other source trees.

5. `.github/workflows/ci.yml`
   - Both `build-and-test` and `coverage` jobs now `apt-get install -y libgtest-dev libgmock-dev`. (Ubuntu 22+ ships these prebuilt.) The vendored fallback handles cases where the package isn't usable.

6. Top-level `CMakeLists.txt`
   - New gtest discovery block right after the spdlog block. Pattern mirrors spdlog:
     - `find_package(GTest CONFIG QUIET)` first, then `find_package(GTest)` (MODULE-mode) for distros where the apt package only ships FindGTest.
     - If neither finds GTest, fall back to `add_subdirectory(third_party/googletest EXCLUDE_FROM_ALL)` and add `GTest::*` aliases for the `gtest`, `gtest_main`, `gmock`, `gmock_main` targets so downstream code sees a single namespaced name regardless of source.
   - New `hft_gtests` executable target. Phase 0 ships only `tests/common/GTestSmoke.cpp`. Links against `hft_lib`, `GTest::gtest`, `GTest::gmock`, `GTest::gtest_main`, plus `twsapi_vendor` + `hft_ibkr_link_deps` when IBKR is enabled (same MinGW link-order constraint as `hft_tests`).
   - Registered with CTest via `add_test(NAME hft_gtests COMMAND hft_gtests)`.

7. `tests/common/GTestSmoke.cpp` (new, ~50 lines)
   - Four smoke tests that exercise gtest, gmock, and `hft_lib` headers end-to-end:
     - `GTestSmoke.BasicAssertion`: confirms gtest itself works (`EXPECT_THAT(... StrEq(...))`).
     - `GTestSmoke.SpscRingRoundTrip`: round-trip `hft::log::SpscRing<int, 8>` push/pop.
     - `GTestSmoke.EventEnumToString`: `hft::log::to_string(AppState::Live)` etc.
     - `GTestSmoke.GmockBasicExpectation`: `MOCK_METHOD` + `EXPECT_CALL` + `WillOnce(Return(...))`.
   - Intentionally tiny. The real unit/module tests land in subsequent phases.

What did NOT change:
- Existing `hft_tests` legacy framework binary is fully preserved. Neither tests nor `tests/common/TestFramework.hpp` were touched.
- No production code (`src/`, `include/`) changed.
- spdlog wiring is unchanged.

Validation done:
- Local UCRT64 build with `HFT_ENABLE_IBKR=ON`: clean. All targets built, including `hft_gtests.exe`.
- Configure log shows the resolution paths exercised: `Using spdlog from find_package: spdlog::spdlog` (from `dependencies/ucrt64/install/lib/cmake/spdlog`) and `Using GTest from find_package: GTest::gtest / GTest::gmock` (from `D:/msys64/ucrt64/lib/cmake/GTest` - MSYS2 system gtest 1.17.0). The vendored fallback for GTest exists but isn't triggered locally because MSYS2 ships gtest; CI on Ubuntu will exercise it via `libgtest-dev`. The vendored copy is the safety net for environments missing both.
- `ctest --output-on-failure`: `hft_gtests` PASSED (4/4 tests). `hft_tests` FAILED with the same 2 pre-existing IBKR-stub broker integration failures (unchanged from previous handoff entries).
- `./hft_gtests.exe` direct run: 4 tests pass in <1 ms total. Both gtest and gmock infrastructure verified.
- Idempotency: re-ran `scripts/stage_third_party_sources_ucrt.sh` after vendoring. All deps including googletest report `present / keep existing`. No re-download.

Known risks / follow-up:
- `find_package(GTest CONFIG)` on MSYS2 UCRT64 currently picks up the system gtest at `D:/msys64/ucrt64/lib/cmake/GTest` (1.17.0) before the deps-prefix install (1.15.2). This is fine because gtest's API is stable across these versions, but if we ever need a specific version, set `GTest_ROOT` or `CMAKE_PREFIX_PATH` to put the deps prefix first. Documented but not changed.
- `hft_tests` continues to FAIL the same 2 IBKR-stub integration tests it has been failing. Phase 1 will make `IBKRClient` mockable, which lets us replace those TCP-attempt tests with mock-driven UTs. Phase 0 intentionally does not touch them.
- `ci.yml` apt-installs `libgtest-dev libgmock-dev` but does not yet add a CTest invocation that runs `hft_gtests` separately. The existing `ctest --output-on-failure` step will pick it up automatically; no workflow change needed.

Suggested commit (simple):
```
git add third_party/googletest \
        scripts/stage_third_party_sources_ucrt.sh \
        scripts/build_third_party_dependencies_ucrt.sh \
        scripts/rebuild_linux_deps_ci.sh \
        .github/workflows/ci.yml \
        CMakeLists.txt \
        tests/common/GTestSmoke.cpp \
        agent/AGENT_HANDOFF_LOG.md

git commit -m "test: stage googletest + add hft_gtests smoke target"
```

---

## [2026-04-28] - Drop HFT_ENABLE_STATE_LOGGING option and push state ownership into subsystems

Model / agent:
- Claude Opus 4.7, reasoning model (Cursor agent)

Source state:
- Local working tree at `D:\trading-system` (git repo, branch `main`).
- Builds on top of `8aeaf39` "feat(log): add state-centric logging (spdlog + SPSC ring + writer thread)" which CI confirmed green (push to main, all four jobs `success`).

User request:
- "I'm kind of annoyed with HFT_ENABLE_STATE_LOGGING I would like to replace. Do wider adoption of the logging api. I refreshed. CI passes, you can also check."
- Goal: make logging unconditional (no opt-out flag) and propagate the state-tracking API into the actual subsystems instead of having `main.cpp` poke component state on behalf of components it doesn't own.

What changed:

1. CMake: spdlog is now a hard dependency
   - `CMakeLists.txt`: removed `option(HFT_ENABLE_STATE_LOGGING ...)` and the surrounding `if(HFT_ENABLE_STATE_LOGGING) ... endif()` block. spdlog discovery (find_package, fallback to vendored `third_party/spdlog`) and the `target_sources(hft_lib PRIVATE LoggingService.cpp LoggingState.cpp)` + `target_link_libraries(hft_lib PUBLIC spdlog::spdlog)` lines now run unconditionally.
   - The `target_compile_definitions(hft_lib PUBLIC HFT_ENABLE_STATE_LOGGING=1)` line is gone (no longer needed since the macro is no longer referenced anywhere).

2. main.cpp: stripped to lifecycle-only orchestration
   - Removed every `#if defined(HFT_ENABLE_STATE_LOGGING)` guard.
   - `#include "log/logging_state.hpp"` and `namespace hl = hft::log;` are now top-level (no `#if`).
   - main only emits AppState transitions (`Starting`, `LoadingConfig`, `ConnectingBroker`, `Live`, `ShuttingDown`, `Fatal`) and the `Logger Ready` once after `initialize_logging()`. It no longer sets `Engine`/`Universe`/`MarketData`/`Broker` states - those moved into the components themselves.

3. Wider adoption: each subsystem owns its own component state
   - `src/lib/LiveExecutionEngine.cpp`:
     - `start()`: emits `Engine Starting` before `broker_->connect`, `Engine Ready` on success, `Engine Error` (with `raise_error`) on failure.
     - `stop()`: emits `Engine Down`.
     - `initialize_universe()`: emits `Universe Starting -> Ready` around the call.
     - `subscribe_live_books()`: emits `MarketData Starting -> Ready` around the loop.
     - `step(t)`: emits `heartbeat(Engine)` every 100 ticks so `last_update_ns` advances without per-tick log spam.
   - `src/lib/IBKRClient.cpp`:
     - `connect()`: emits `Broker Starting`, then `Broker Ready` on success or `Broker Error` (+ `raise_error` code=2) on failure.
     - `disconnect()`: emits `Broker Down` (only when previously connected, to avoid spurious transitions).
     - `connectionClosed()`: was inline in the header; moved to the .cpp so it can emit `Broker Error` (code=3) + `raise_error("IBKR connection closed by remote")`.
     - Header change: `void connectionClosed() override;` instead of inline body.
   - `include/broker/PaperBrokerSim.hpp`: header-only class, now `#include "log/logging_state.hpp"` and emits `Broker Ready` from `connect()` and `Broker Down` from `disconnect()`.

4. Concurrency fix: producer-side registry update with atomic exchange
   - Symptom found in the first smoke run: every component transition logged with the wrong `old_state` (e.g. `Engine Down -> Starting` immediately followed by `Engine Down -> Ready` instead of `Engine Starting -> Ready`).
   - Root cause: producers read `old_state` from the registry at push time, but the registry was only updated by the writer thread when it later processed the event. Back-to-back pushes from the same thread saw a stale `old_state`.
   - Fix:
     - `include/log/state_registry.hpp`: added `exchange_app_state(...)` and `exchange_component_state(...)` that perform an atomic `exchange` on the underlying `std::atomic<std::uint8_t>` (memory_order_acq_rel), update timestamp/code, and return the prior value. The existing `set_*` methods now delegate to `exchange_*` (discarding the return value).
     - `src/lib/LoggingState.cpp`: `set_app_state` and `set_component_state` now call `exchange_*`, capture the returned prior value, and stamp it into the event's `old_state` field. Subsequent reads (including by other producer threads) immediately see the new state.
     - `src/lib/LoggingService.cpp`: the writer thread no longer calls `registry_.set_*` for `AppStateChanged` / `ComponentStateChanged` (it would be a redundant double-write). It only emits the spdlog line.
   - Net effect (verified in the second smoke run): transitions now read correctly. Sample:
     ```
     APP Starting -> LoadingConfig
     APP LoadingConfig -> ConnectingBroker
     Engine Down -> Starting
     Broker Down -> Ready
     Engine Starting -> Ready
     Universe Down -> Starting
     Universe Starting -> Ready
     MarketData Down -> Starting
     MarketData Starting -> Ready
     APP ConnectingBroker -> Live
     ```

What did NOT change:
- All other subsystems (`RankingEngine`, `AppConfig`, validation, simulator, etc.) - the API is now adopted at the subsystem-state boundaries (engine/broker/universe/market-data) and there's no intent to spam transitions per-step.
- spdlog vendoring under `third_party/spdlog/` - same files, same tag.
- Build scripts (`scripts/stage_third_party_sources_ucrt.sh`, `scripts/build_third_party_dependencies_ucrt.sh`, `scripts/rebuild_linux_deps_ci.sh`) and `.github/workflows/ci.yml` - all still install/stage spdlog the same way.
- Test code - no test relied on `HFT_ENABLE_STATE_LOGGING`, so no test had to change.

Errors encountered + fixes:
- First smoke run after the refactor showed `set_app_state` reporting stale `old_state` (described above). Fixed via atomic `exchange_*` on the producer side; the writer thread now only formats. Verified by a second rebuild + smoke run.
- (Side note) `_run_state_logging_smoke.sh` does not rebuild - first time I noticed I had to call `agent/_run_project_build.sh` before re-smoking. Not changing the script for now since it's intentionally smoke-only.

Validation done:
- `agent/_run_project_build.sh`: project builds clean with `HFT_ENABLE_IBKR=ON` (build dir `build-ucrt-ibkr`). All targets link, including `hft_app.exe`, `hft_tests.exe`, and `libhft_lib.a`.
- `agent/_run_state_logging_smoke.sh`:
  - `hft_tests.exe`: same 118 PASS / 2 FAIL as before this change (the 2 failures are the pre-existing IBKR-stub broker integration tests; unrelated).
  - `hft_app.exe` paper-mode smoke: emits the correct sequence of subsystem transitions shown above, then 1 Hz `HEALTH` summaries from the writer thread. The 12 s `timeout` cap returns rc=124 which is expected for an engine running 500 steps; not a logging issue.
- Lints: `ReadLints` clean on every changed file (CMakeLists, main.cpp, LiveExecutionEngine.cpp, IBKRClient.cpp/.hpp, PaperBrokerSim.hpp, state_registry.hpp, LoggingState.cpp, LoggingService.cpp).
- CI for the parent commit `8aeaf39`: all four jobs green (verified via the GitHub REST API at the start of the session).

Known risks / follow-up:
- No test yet exercises the `exchange_app_state` / `exchange_component_state` race-fix directly. A small unit test that hammers `set_component_state` from two threads and asserts that every produced event has a self-consistent `old_state -> new_state` pair would lock this in.
- `IBKRClient::orderStatus` and `updateMktDepth` could optionally heartbeat the `Broker` / `MarketData` components so the registry's `last_update_ns` reflects real activity rather than just startup. Skipped for now to avoid log volume issues; revisit when adding a "stale market data" alarm.
- `start_production_event_loop` and `reconnect_once` are good places for `WarningRaised` events if reconnect is taking a while. Skipped here to keep the diff focused on the ownership refactor.
- `connectionClosed` now requires `HFT_ENABLE_IBKR` (the implementation is in the `#ifdef HFT_ENABLE_IBKR` block in IBKRClient.cpp). The header-side declaration is unguarded which is fine because the EWrapper override only exists when `HFT_ENABLE_IBKR` is defined - in non-IBKR builds nothing references it. Verified by the IBKR=ON build link succeeding.

Suggested commit:
```
git add CMakeLists.txt src/app/main.cpp src/lib/LiveExecutionEngine.cpp \
        src/lib/IBKRClient.cpp src/lib/LoggingState.cpp src/lib/LoggingService.cpp \
        include/broker/IBKRClient.hpp include/broker/PaperBrokerSim.hpp \
        include/log/state_registry.hpp agent/AGENT_HANDOFF_LOG.md
git commit -m "refactor(log): drop HFT_ENABLE_STATE_LOGGING and push state ownership into subsystems"
```

Suggested commit message body:
```
- spdlog is now a hard, unconditional dependency of hft_lib; the CMake option
  is gone, all #if defined(HFT_ENABLE_STATE_LOGGING) guards are gone, and the
  HFT_ENABLE_STATE_LOGGING=1 compile definition is no longer emitted.
- main.cpp is now lifecycle-only: it owns AppState transitions and Logger
  readiness, nothing else.
- LiveExecutionEngine emits Engine/Universe/MarketData state itself.
- IBKRClient and PaperBrokerSim emit Broker state themselves.
- IBKRClient::connectionClosed moved out-of-line so it can emit Broker Error +
  raise_error on remote disconnect.
- Producer-side atomic exchange on the state registry so transitions display
  the correct old_state even with rapid back-to-back set_*_state calls.
```

---

## [2026-04-28] - Add state-centric logging module (spdlog + SPSC ring + background writer) and wire it into UCRT/CI builds

Model / agent:
- Claude Opus 4.7, reasoning model (Cursor agent)

Source state:
- Local working tree at `D:\trading-system` (git repo, branch `feat/multi-asset-trading-system`).
- MSYS2 UCRT64 toolchain at `D:\msys64\ucrt64` with gcc/g++ 15.2.0 and CMake 4.1.2.
- Starting point: clean tree at `c1b0bb9` "Added scripts for Cursor". The previous session left the project building `hft_app`/`hft_tests` with `HFT_ENABLE_IBKR=ON`.

User request:
- Read `chat_export_exact.txt` from Downloads (a ChatGPT conversation about HFT-style logging in C++) and add logging to the project.
- The chat specified: spdlog as the operational logging library, fixed-size structured event types, lock-free SPSC ring buffer, background writer thread, and a state registry so callers can answer "what state is the application in right now?".
- Integrate with both CI (Ubuntu) and the UCRT build, keeping the existing dependency-staging workflow intact.

What was added (new files, all under the `hft::log` namespace):
- `include/log/event_types.hpp` - `AppState`, `ComponentState`, `ComponentId`, `EventType` enums, plus POD event structs (`EventHeader`, `AppStateChangedEvent`, `ComponentStateChangedEvent`, `HeartbeatEvent`, `WarningRaisedEvent`, `ErrorRaisedEvent`). All events are trivially copyable so producers can memcpy them into a ring buffer with no heap allocation.
- `include/log/spsc_ring.hpp` - header-only bounded single-producer/single-consumer ring (`SpscRing<T, Capacity>`) using cache-line-padded atomics. Reserves one slot to disambiguate full/empty.
- `include/log/state_registry.hpp` - in-memory snapshot of the current app + per-component state, exposed via relaxed atomics. This is the "where are we now?" source of truth.
- `include/log/logging_service.hpp` / `src/lib/LoggingService.cpp` - background writer thread. Owns one `EventQueue` per producer (lazily created via `service().thread_queue()`), drains all registered queues, decodes each event, mirrors important transitions to spdlog (async logger with file + console sinks), and emits a periodic `HEALTH ...` summary line at 1 Hz.
- `include/log/logging_state.hpp` / `src/lib/LoggingState.cpp` - tiny producer-side API: `initialize_logging`, `shutdown_logging`, `set_app_state`, `set_component_state`, `heartbeat`, `raise_warning`, `raise_error`. Each call is non-blocking: it stamps a steady_clock ts, fills a fixed-size event, and `try_push`es into the per-thread queue. If the singleton has not been initialized the calls are silent no-ops, so accidental use before init never crashes.

Wiring and integration changes:
- `CMakeLists.txt` - appended a new block at the end (does not touch the IBKR block above):
  - Adds `option(HFT_ENABLE_STATE_LOGGING ...)` defaulting ON.
  - Tries `find_package(spdlog CONFIG QUIET)` first; falls back to `add_subdirectory(third_party/spdlog EXCLUDE_FROM_ALL)`; otherwise emits `FATAL_ERROR` listing the install/staging options.
  - Adds the two new sources to `hft_lib` via `target_sources(... PRIVATE ...)` and links `hft_lib PUBLIC spdlog::spdlog`.
  - Adds `target_compile_definitions(hft_lib PUBLIC HFT_ENABLE_STATE_LOGGING=1)` so consumers can `#if defined(HFT_ENABLE_STATE_LOGGING)`-gate calls.
- `src/app/main.cpp` - additive only. All existing `std::cout` lines are preserved; the new state calls are guarded by `#if defined(HFT_ENABLE_STATE_LOGGING)`. Calls cover Starting -> LoadingConfig -> ConnectingBroker -> Live transitions, plus per-component `set_component_state` for `Logger`, `Broker`, `Engine`, `Universe`, `MarketData`. On the early-return error path and at end-of-main the app calls `shutdown_logging()` so the writer thread drains cleanly.
- `scripts/stage_third_party_sources_ucrt.sh`:
  - Adds `SPDLOG_TAG=v1.15.3` and `SPDLOG_REPO_URL=https://github.com/gabime/spdlog.git` (overridable via env).
  - New `has_spdlog()` validator (checks `CMakeLists.txt`, `include/spdlog/spdlog.h`, `include/spdlog/async.h`).
  - New `download_spdlog()` that does `git clone --branch ${SPDLOG_TAG} --depth 1` into `third_party/_downloads/spdlog`, then `mv` into `third_party/spdlog`. Validates the tree before commit.
  - `FORCE_RESTAGE_DEPS=1` now also wipes `third_party/spdlog`.
  - Final summary block reports `spdlog: present|missing` and the existence check is part of the "required" gate.
- `scripts/build_third_party_dependencies_ucrt.sh`:
  - Adds `SPDLOG_SRC` variable + `test -d` guard.
  - **Removed the `bid_binarydecimal.c` exclusion in the generated Intel RDFP wrapper CMakeLists.** The previous comment claimed the file "does not compile cleanly under MinGW/UCRT", but a standalone compile under UCRT64 GCC 15.2.0 with the script's own `-DCALL_BY_REF=0 -DGLOBAL_RND=0 -DGLOBAL_FLAGS=0 -DUNCHANGED_BINARY_FLAGS=0` produced a valid 2 MB object that exports `T __bid64_to_binary64` and `T __binary64_to_bid64` (the symbols TWS API's `Decimal.cpp` calls directly). Without those symbols defined, the symbol-verification step failed and any IBKR project link would have failed too. Including the file fixed both. See "Errors and root causes" below.
  - Updated the comment block in the generated wrapper to explain why `bid_binarydecimal.c` is now in.
  - After Intel RDFP, added a `cmake -S third_party/spdlog -B build/spdlog -G "Unix Makefiles" -DSPDLOG_BUILD_SHARED=OFF -DSPDLOG_BUILD_EXAMPLE=OFF -DSPDLOG_BUILD_TESTS=OFF -DSPDLOG_INSTALL=ON -DCMAKE_INSTALL_PREFIX=...` configure/build/install step. Verifies `dependencies/ucrt64/install/lib/libspdlog.a` was produced.
- `scripts/rebuild_linux_deps_ci.sh`:
  - Adds `SPDLOG_TAG`, `SPDLOG_REPO_URL`, `SPDLOG_SRC` and includes spdlog in the up-front cleanup.
  - Adds a `git clone --branch ${SPDLOG_TAG} --depth 1` step + `cmake -S ... -DSPDLOG_BUILD_SHARED=ON -DCMAKE_POSITION_INDEPENDENT_CODE=ON ...` build/install into the same `dependencies/linux/install` prefix. The Linux dependency tarball uploaded to the `linux-deps` GitHub release will now contain spdlog headers + shared library, so the `ibkr-build` CI job (which downloads that asset) gets spdlog "for free".
  - Updated the `DRY_RUN` summary message to mention spdlog.
- `.github/workflows/ci.yml`:
  - The `build-and-test` and `coverage` jobs now `apt-get install -y libspdlog-dev` so `find_package(spdlog CONFIG)` succeeds without the dependency bundle.
  - The `linux-deps-release` and `ibkr-build` jobs are unchanged at the workflow level; they consume the bundle that `rebuild_linux_deps_ci.sh` now builds, which already includes spdlog.

Files added (not version-controlled, gitignored under `agent/_*.sh`):
- `agent/_run_deps_build.sh` - wraps `bash scripts/build_third_party_dependencies_ucrt.sh` with full output redirected to `_build_deps.log`. Required because PowerShell intercepts `>` redirections that we tried to chain inline through `D:\msys64\usr\bin\bash.exe -c "..."`.
- `agent/_run_project_build.sh` - clean re-config + build of `hft_app`/`hft_tests` against the UCRT install prefix, with output captured to `_project_build.log`.
- `agent/_run_state_logging_smoke.sh` - runs `hft_tests.exe` then a 12-second timeout-bounded `hft_app.exe` to confirm `logs/app.log` is being produced.

Files removed / not committed:
- Intermediate scratch artifacts created during diagnosis (`agent/_test_bid_bd.sh`, `agent/_inspect_obj.sh`, `_*.log`, `test_bid_bd.o`) were deleted at end of session. None of them were ever committed; the agent helper scripts are gitignored via `agent/_*.sh`.

Errors encountered and root causes:
1. PowerShell's outer shell ate `$PATH` and `>` redirections when invoking `D:\msys64\usr\bin\bash.exe --login -c "..."`. Worked around by writing all multi-step bash work into local helper scripts under `agent/_*.sh` and invoking them with `D:\msys64\usr\bin\bash.exe --login /d/trading-system/agent/_<name>.sh`.
2. After the dependency build's first run, `__bid64_to_binary64` was not exported by `libintelrdfpmath.a` and the symbol-verification loop in `scripts/build_third_party_dependencies_ucrt.sh` exited 1. Investigation showed that the upstream Intel RDFP source `bid_binarydecimal.c` (the only file that defines those symbols) was being explicitly excluded by `list(FILTER RDFP_SOURCES EXCLUDE REGEX ".*[/\\]bid_binarydecimal\\.c$")` in the generated wrapper CMakeLists. The comment claimed "does not compile cleanly in this MinGW/UCRT CMake wrapper", but a direct `gcc -c -O3 -DCALL_BY_REF=0 -DGLOBAL_RND=0 -DGLOBAL_FLAGS=0 -DUNCHANGED_BINARY_FLAGS=0 -I third_party/IntelRDFPMathLib20U4 bid_binarydecimal.c` produced a valid object with all expected symbols. Removing the exclusion line and the matching `echo "==> Excluding optional bid_binarydecimal.c"` line fixed the verification, restored `__bid64_to_binary64` / `__binary64_to_bid64`, and is required for the IBKR-enabled project to link `Decimal.cpp` cleanly. The previous handoff entry's claim that those two symbols were "verified" was inaccurate; the verification only "passed" in earlier sessions because grep was matching unrelated lines containing "binary64" or because the install prefix had stale objects from before the wrapper was changed.

Validation performed:
- `bash scripts/stage_third_party_sources_ucrt.sh` -> exit 0; `third_party/spdlog/include/spdlog/spdlog.h` and `CMakeLists.txt` present.
- `bash scripts/build_third_party_dependencies_ucrt.sh` -> exit 0; produced `dependencies/ucrt64/install/lib/{libabsl_*.a, libintelrdfpmath.a, libprotobuf*.a, librt.a, libspdlog.a, libupb.a, libutf8_*.a}` and the corresponding includes (`spdlog/`, `bid*.h`, `absl/`, `google/`, `upb/`, etc.).
- `nm -g .../libintelrdfpmath.a | grep -E 'T __bid64_to_binary64|T __binary64_to_bid64'` -> both present (was empty before this session's fix).
- `cmake -S . -B build-ucrt-ibkr -G "Unix Makefiles" -DHFT_ENABLE_IBKR=ON -DHFT_ENABLE_STATE_LOGGING=ON -DCMAKE_PREFIX_PATH=...` -> configure done; CMake reports "Using spdlog from find_package: spdlog::spdlog" (resolved via `dependencies/ucrt64/install/lib/cmake/spdlog`).
- `cmake --build build-ucrt-ibkr -j$(nproc)` -> exit 0. Built `libtwsapi_vendor.a`, `libhft_lib.a`, `hft_app.exe` (8.7 MB), `hft_tests.exe` (8.1 MB) on the first try with no warnings beyond the pre-existing third-party noise.
- `grep HFT_ENABLE_IBKR build-ucrt-ibkr/CMakeCache.txt` -> `HFT_ENABLE_IBKR:BOOL=ON` and `HFT_ENABLE_STATE_LOGGING:BOOL=ON`.
- `./hft_tests.exe` -> 118 PASS / 2 FAIL. The 2 failures are exactly the pre-existing `test_live_execution_engine_with_ibkr_stub_reconcile_path` / `test_live_execution_engine_live_mode_uses_ibkr_stub` tests that need a running IBKR stub. No new failures were introduced.
- 12-second smoke run of `hft_app.exe` in paper mode produced `logs/app.log` containing the full state-transition stream:
  ```
  [2026-04-28 03:52:20.208] [hft_ops] [info] LoggingService started
  [2026-04-28 03:52:20.208] [hft_ops] [info] APP Starting -> LoadingConfig code=0
  [2026-04-28 03:52:20.208] [hft_ops] [info] Broker Down -> Starting code=0
  [2026-04-28 03:52:20.208] [hft_ops] [info] APP Starting -> ConnectingBroker code=0
  [2026-04-28 03:52:20.208] [hft_ops] [info] Broker Down -> Ready code=0
  [2026-04-28 03:52:20.208] [hft_ops] [info] Engine Down -> Ready code=0
  [2026-04-28 03:52:20.208] [hft_ops] [info] Universe Down -> Ready code=0
  [2026-04-28 03:52:20.208] [hft_ops] [info] MarketData Down -> Ready code=0
  [2026-04-28 03:52:20.208] [hft_ops] [info] APP Starting -> Live code=0
  [2026-04-28 03:52:21.213] [hft_ops] [info] HEALTH app=Live md=Ready broker=Ready risk=Down strategy=Down engine=Ready universe=Ready
  ...
  ```
  Periodic 1 Hz `HEALTH` summary lines and individual transitions both confirmed.

Known risks / follow-up:
- `third_party/spdlog/` is **vendored** at tag `v1.15.3` (commit `6fa36017cfd5731d617e1a934f0e5ea9c4445b13`), 174 files / 1.5 MB. This matches how `third_party/IntelRDFPMathLib20U4/` and `third_party/abseil-cpp/` are committed and decouples builds from network access. The clone's `.git` directory was removed before staging so git treats the tree as plain vendored sources, not a submodule. The staging script's `has_spdlog()` validator continues to pass for the vendored tree and the `download_spdlog()` path is only exercised on a missing/corrupted tree, so CI/UCRT flows still work unchanged.
- `LoggingService::Config` is currently honored only on first construction. The second `initialize_logging(cfg)` call after a singleton has been created silently ignores the new cfg; the comment in `LoggingState.cpp` documents this. If the project later needs reconfigure-after-init, the singleton needs to grow a "swap config under lock" path.
- The first state event the application emits is "APP Starting -> Starting" because `set_app_state(Starting)` runs before any transition. Cosmetic only. Easy follow-up: skip pushing an event when `old == new`.
- spdlog's async writer thread is independent of the producer-side SPSC ring + writer thread. The HEALTH summary lines are emitted by the writer thread; the `[hft_ops] [info]` lines are then handed to spdlog's own async pool. Two indirections of buffering means the on-disk order of log lines vs `std::cout` lines is not strictly causal, but the absolute timestamps in `[%Y-%m-%d %H:%M:%S.%e]` are accurate.
- The 2 IBKR-stub test failures are unchanged from the previous session and predate this work.

Suggested commit:
```bash
git add CMakeLists.txt src/app/main.cpp \
        include/log/event_types.hpp include/log/spsc_ring.hpp \
        include/log/state_registry.hpp include/log/logging_service.hpp \
        include/log/logging_state.hpp \
        src/lib/LoggingService.cpp src/lib/LoggingState.cpp \
        scripts/stage_third_party_sources_ucrt.sh \
        scripts/build_third_party_dependencies_ucrt.sh \
        scripts/rebuild_linux_deps_ci.sh \
        .github/workflows/ci.yml \
        third_party/spdlog \
        agent/AGENT_HANDOFF_LOG.md
git commit -m "feat(log): add state-centric logging (spdlog + SPSC ring + writer thread)

* New module under include/log + src/lib/Logging*.cpp:
  - event_types.hpp: AppState/ComponentState/ComponentId/EventType + POD events
  - spsc_ring.hpp: header-only bounded single-producer/single-consumer ring
  - state_registry.hpp: atomic snapshot answering 'what state is the app in?'
  - LoggingService: background writer thread, drains per-thread queues,
    forwards transitions to spdlog (async logger, file + console sinks)
  - LoggingState: producer API (set_app_state, set_component_state,
    heartbeat, raise_warning, raise_error)
* CMakeLists: HFT_ENABLE_STATE_LOGGING option (default ON), find_package
  spdlog with vendored fallback, links spdlog::spdlog into hft_lib.
* src/app/main.cpp: additive set_app_state / set_component_state calls
  alongside existing std::cout lines, gated by HFT_ENABLE_STATE_LOGGING.
* scripts/stage_third_party_sources_ucrt.sh: stages spdlog v1.15.3 from
  github.com/gabime/spdlog into third_party/spdlog.
* scripts/build_third_party_dependencies_ucrt.sh: builds spdlog static into
  dependencies/ucrt64/install. Also includes bid_binarydecimal.c in the
  Intel RDFP wrapper build (was wrongly excluded; the file compiles fine
  under UCRT64 GCC and is the source of __bid64_to_binary64 / __binary64_to_bid64
  that TWS API's Decimal.cpp calls directly).
* scripts/rebuild_linux_deps_ci.sh: clones+builds spdlog into the Linux
  dependency bundle so the ibkr-build CI job inherits spdlog from the
  release asset. Non-IBKR Ubuntu jobs install libspdlog-dev directly.

Verified end-to-end on UCRT64: hft_app.exe (8.7 MB) and hft_tests.exe
(8.1 MB) build with HFT_ENABLE_IBKR=ON HFT_ENABLE_STATE_LOGGING=ON.
hft_tests: 118 PASS / 2 FAIL (pre-existing IBKR-stub-connect tests).
hft_app smoke run produces logs/app.log with state transitions and
1 Hz HEALTH summary lines."
```

## [2026-04-28] - Fix UCRT third-party build end-to-end and produce hft_app/hft_tests with HFT_ENABLE_IBKR=ON

Model / agent:
- Claude Opus 4.7, reasoning model (Cursor agent)

Source state:
- Local working tree at `D:\trading-system` (git repo). No clone or fetch performed.
- MSYS2 UCRT64 toolchain at `D:\msys64\ucrt64` with gcc/g++ 15.2.0 and CMake 4.1.2.
- Initial state: `scripts/build_third_party_dependencies_ucrt.sh` was failing.

User request:
- Diagnose and fix `scripts/build_third_party_dependencies_ucrt.sh` failures under MSYS2 UCRT64.
- Then build the actual project targets (`hft_app`, `hft_tests`) with `HFT_ENABLE_IBKR=ON`.

Files changed:
- `scripts/build_third_party_dependencies_ucrt.sh`
  - Replaced removed `MinGW Makefiles` generator with `Unix Makefiles` in all three CMake configure calls (Abseil, protobuf, Intel RDFP wrapper) and in the printed "next step" hint. CMake 4.1.2 dropped `MinGW Makefiles`.
  - Added a UCRT64 toolchain-detection block at the top: when `/ucrt64/bin/gcc.exe` exists, exports `CC`/`CXX` to the Windows paths produced by `cygpath -m` and prepends `/ucrt64/bin` to `PATH`. This avoids `bash --login` picking up `/usr/bin/gcc` (Cygwin GCC), which defines `__CYGWIN__` and made Abseil's `policy_checks.h` refuse to compile.
  - Replaced hardcoded `-DCMAKE_C_COMPILER=gcc`/`-DCMAKE_CXX_COMPILER=g++` with `${CC:-gcc}`/`${CXX:-g++}` so the detected UCRT64 compiler is honored.
  - Changed Abseil from `BUILD_SHARED_LIBS=ON` to `OFF` (MinGW shared-build of Abseil pulls `-lrt`, which does not exist on Windows; static archives sidestep this).
  - Added `CMAKE_EXE_LINKER_FLAGS`/`CMAKE_SHARED_LINKER_FLAGS="-L${INSTALL_DIR}/lib"` for both Abseil and protobuf so their builds can find a librt stub.
  - Created an empty `librt.a` stub via `ar qc "${INSTALL_DIR}/lib/librt.a"` to satisfy `-lrt` references that Abseil's and protobuf's installed CMake configs unconditionally export, even on MinGW.
  - **Removed the `tws_bid_compat.c` shim** entirely from the generated Intel RDFP wrapper CMakeLists.txt. Verified with `nm` that the upstream sources already export `__bid64_add`, `__bid64_sub`, `__bid64_mul`, `__bid64_div`, `__bid64_from_string`, `__bid64_to_string`, `__bid64_to_binary64`, `__binary64_to_bid64` directly — the compat shim was both unnecessary and wrong (it referenced single-underscore `bid64_*` names that don't exist).
  - Symbol verification now runs `nm` against `libintelrdfpmath.a` directly. Any stale `libintelrdfpmath_compat.a` from earlier runs is removed automatically.
- `scripts/stage_third_party_sources_ucrt.sh`
  - **Root-cause hardening:** the script previously trusted any `curl` exit-zero. A prior run had produced a 0-byte `IntelRDFPMathLib20U4.tar.gz`, which silently extracted into 225 empty `.c` files. The build then compiled empty objects and the final link could not find any `__bid64_*` symbols.
  - `has_rdfp()` now also requires ≥ 50 non-empty `.c` files in the staged tree.
  - Added an `INTEL_DEC_URLS_FALLBACK` array (netlib + a web.archive.org mirror) and a download loop with `curl -fL --retry 3`, post-download size validation (≥ 1 MiB) before declaring success.
- `CMakeLists.txt`
  - Linked the IBKR build against a single Intel decimal archive (`HFT_INTEL_RDFP_LIBRARY` -> `libintelrdfpmath.a`). An earlier transitional commit in this session had added an optional `find_library(HFT_INTEL_RDFP_COMPAT_LIBRARY ...)` plus a conditional link clause. That branch was removed at end of session because the upstream RDFP archive already exports `__bid64_*` directly; only one Intel archive is referenced anywhere in the build.

Files added / moved:
- `agent/_configure_ucrt.sh`, `agent/_build_ucrt.sh`, `agent/_run_tests.sh` - local agent helpers that wrap the documented `cmake -S . -B build-ucrt-ibkr -G "Unix Makefiles" -DHFT_ENABLE_IBKR=ON ...` flow with the correct UCRT64 compiler resolution (via `cygpath -m`). Live under `agent/` next to the workflow/log files. Gitignored (`agent/_*.sh`), so they stay machine-local. Not part of the blessed CI flow.
- `agent/enforce_additive_only.py` - moved from `scripts/enforce_additive_only.py` via `git mv` to keep all agent-facing tooling in one place. No call sites in workflows, scripts, or docs reference the old path, so no further updates were needed.
- `.gitignore` - added `agent/_*.sh` to keep the underscore-prefixed agent helpers out of version control. Verified with `git check-ignore -v agent/_configure_ucrt.sh agent/_build_ucrt.sh agent/_run_tests.sh`.

Deletions / removals:
- Removed the `TWS_BID_COMPAT_SOURCE` heredoc and the `intelrdfpmath_compat` static library target from the generated CMakeLists block in `scripts/build_third_party_dependencies_ucrt.sh`. Approximate range: the `set(TWS_BID_COMPAT_SOURCE ...)`/`file(WRITE ...)` block, the `add_library(intelrdfpmath_compat STATIC ...)` target and its `target_link_libraries`, and the `intelrdfpmath_compat` entry in the `install(TARGETS ...)` list. The single `__bid64_*` verification loop was kept but retargeted at `libintelrdfpmath.a` directly.
- Deleted at runtime by the script: any pre-existing `dependencies/ucrt64/install/lib/libintelrdfpmath_compat.a`.
- Removed at the user's machine to remediate the corrupted state: `third_party/IntelRDFPMathLib20U4/` (225 empty `.c` files) and `third_party/_downloads/IntelRDFPMathLib20U4.tar.gz` (0 bytes). Both were re-staged from netlib (5.8 MB archive, 226 non-empty sources).
- No source code under `src/`, `include/`, or `tests/` was modified or removed.

Steps taken:
1. Ran the dependency build script and saw `Could not create named generator MinGW Makefiles` from CMake 4.1.2. Replaced with `Unix Makefiles`.
2. Re-ran and saw Abseil's `policy_checks.h` reject the build with `#error "Cygwin is not supported."` because the `--login` shell put `/usr/bin/gcc` (Cygwin) ahead of `/ucrt64/bin/gcc`. Added UCRT64 detection at the top of the script and routed cmake's `CMAKE_C_COMPILER`/`CMAKE_CXX_COMPILER` through `cygpath -m` so they got Windows paths with `.exe`.
3. Re-ran and saw `cannot find -lrt` while linking Abseil's shared libraries. Switched Abseil to static (`BUILD_SHARED_LIBS=OFF`) and added a stub `librt.a` to the install prefix so the protobuf link line (which still lists `-lrt`) succeeds.
4. Dependency build now finished cleanly; verified by listing `dependencies/ucrt64/install/lib/`.
5. Configured the project with `HFT_ENABLE_IBKR=ON` against the install prefix and ran the build. Got persistent `undefined reference to bid64_to_string` (single underscore) coming from `libintelrdfpmath_compat.a(tws_bid_compat.c.obj)`.
6. Tried split archives + start/end-group; problem persisted because the references themselves were wrong, not just the order.
7. Inspected `nm` output for `bid64_string.c.obj` and found the `.text` section was 0 bytes. Then `find third_party/IntelRDFPMathLib20U4/src -type f -name '*.c' -size 0 | wc -l` returned 225 of 226. The downloaded tarball was 0 bytes — the original failure mode all along.
8. Hardened the staging script with size validation and fallback URLs. Removed the corrupted tree and re-staged: netlib delivered 5.8 MB this time and the extracted source files were correct.
9. With the actual sources present, ran `nm` on the rebuilt `libintelrdfpmath.a` and confirmed `T __bid64_add`, `T __bid64_to_string`, `T __bid64_from_string` are exported directly. The compat shim is therefore unnecessary; it was also wrong because it forwarded to single-underscore names that the upstream library does not produce. Removed it from the wrapper CMakeLists.
10. Cleaned `build-ucrt-ibkr/`, reconfigured with `HFT_ENABLE_IBKR=ON`, and rebuilt.

Validation performed:
- `bash scripts/build_third_party_dependencies_ucrt.sh` -> exit 0; produced `dependencies/ucrt64/install/lib/{libabsl_*.a, libprotobuf*.a, libintelrdfpmath.a, libupb.a, libutf8_*.a}` and the librt stub.
- Symbol verification inside the script passed for all of `__bid64_add`, `__bid64_sub`, `__bid64_mul`, `__bid64_div`, `__bid64_from_string`, `__bid64_to_string`, `__bid64_to_binary64`, `__binary64_to_bid64` against `libintelrdfpmath.a`.
- `cmake -S . -B build-ucrt-ibkr -G "Unix Makefiles" -DHFT_ENABLE_IBKR=ON -DCMAKE_PREFIX_PATH=dependencies/ucrt64/install ...` -> configure done.
- `cmake --build build-ucrt-ibkr -j$(nproc)` -> exit 0. Built `libtwsapi_vendor.a`, `libhft_lib.a`, `hft_app.exe` (7.8 MB), `hft_tests.exe` (8.1 MB).
- `grep HFT_ENABLE_IBKR build-ucrt-ibkr/CMakeCache.txt` -> `HFT_ENABLE_IBKR:BOOL=ON`.
- `nm libhft_lib.a | grep -ci 'IBKR\|TWS\|EClient'` -> 809 (IBKR code paths are in the library).
- `./hft_tests.exe` -> 88 PASS, 12 FAIL. The 12 failures are exclusively IBKR stub connect/lifecycle tests (`test_ibkr_stub_*`, `test_live_execution_engine_*`) failing on `stub connect should succeed`. They are not build issues; they need a running TWS gateway / configured stub. The non-IBKR test suite (core models, math, sim, validation, app config) is fully green.

Known risks / follow-up:
- The librt stub is empty. If any upstream code path actually calls a POSIX real-time function (e.g. `clock_gettime`) the link will succeed but emit an unresolved symbol at runtime. On MinGW-w64 those symbols are provided by UCRT (`pthread_*`, `clock_gettime`, etc.) so this is fine in practice for Abseil/protobuf. If a future dependency genuinely needs `librt`, replace the stub with a thin shim that aliases to the UCRT equivalents.
- The `web.archive.org` fallback URL for Intel RDFP is best-effort. Users behind strict proxies should set `INTEL_DEC_URL` to a known-good mirror.
- The single-Intel-archive setup assumes the RDFP upstream sources continue to export `__bid64_*` symbols directly. The dependency build script's `nm` verification step is the canary and will fail loudly if a future RDFP release changes that. End-to-end re-verification at end of session: clean configure + build with only `libintelrdfpmath.a` present produced `hft_app.exe` (7.8 MB) and `hft_tests.exe` (8.1 MB) without errors.
- The 12 IBKR-stub test failures should be triaged separately. They predate this session's changes.

Suggested commit:
```bash
git add scripts/build_third_party_dependencies_ucrt.sh scripts/stage_third_party_sources_ucrt.sh CMakeLists.txt agent/AGENT_HANDOFF_LOG.md
git commit -m "fix(ucrt): unbreak third-party build for CMake 4.1 + UCRT64 GCC

- replace removed MinGW Makefiles generator with Unix Makefiles
- prefer /ucrt64/bin gcc/g++ via cygpath so Abseil does not see __CYGWIN__
- build Abseil static and ship a librt stub to satisfy MinGW link of -lrt
- validate Intel RDFP archive size and source count to catch silent 0-byte downloads
- drop redundant tws_bid_compat shim; upstream RDFP already exports __bid64_*"
```

## [2026-04-28] - Enforce mandatory model identity in workflow and handoff log

Model / agent:
- GPT-5.5 Thinking, reasoning model

Source state:
- Based on the previously generated `AGENT_WORKFLOW.md` and `AGENT_HANDOFF_LOG.md` from this conversation.

User request:
- Fix the workflow/handoff files because model name and version are mandatory and must not be written as unknown.

Files changed:
- `AGENT_WORKFLOW.md`
  - added a mandatory model identity section
  - clarified that `unknown` must not be used for the model field
  - updated the handoff template to require exact model name/version and model type
- `AGENT_HANDOFF_LOG.md`
  - added this corrective handoff entry
  - preserved existing historical entries with explicit model identity

Deletions / removals:
- No files removed.
- No historical handoff entries removed.

Steps taken:
1. Clarified that the model identity field is mandatory.
2. Added the exact model identity for this interaction: `GPT-5.5 Thinking, reasoning model`.
3. Kept `unknown` only where date/source state was genuinely unavailable in older entries.

Validation performed:
- Generated both Markdown files.
- Packaged both files into a zip.

Known risks / follow-up:
- Future agents must update `AGENT_HANDOFF_LOG.md` after substantive modifications and must include exact model identity.

Suggested commit:
```bash
git add AGENT_WORKFLOW.md AGENT_HANDOFF_LOG.md
git commit -m "docs: require model identity in agent handoff log"
```

## [2026-04-28] - Split agent workflow and handoff log

Model / agent:
- GPT-5.5 Thinking, reasoning model

Source state:
- Based on the previous combined `AGENT_WORKFLOW.md` produced in this conversation.

User request:
- Separate the workflow from the handoff log.
- Create a new handoff log file.
- Note in the workflow that agents should update the handoff log after each substantive chat interaction with the user, such as requests for more modifications, but not confirmations or simple questions.

Files changed:
- `AGENT_WORKFLOW.md`
  - converted into a stable workflow guide only
  - added required handoff behavior
  - clarified when agents should and should not update the handoff log
- `AGENT_HANDOFF_LOG.md`
  - new append-only handoff log file
  - moved prior log-style content into this separate file

Deletions / removals:
- Removed embedded historical handoff entries from `AGENT_WORKFLOW.md`
- No files removed

Steps taken:
1. Split the combined workflow/log structure into two Markdown files.
2. Added an explicit rule that agents should append to `AGENT_HANDOFF_LOG.md` after substantive user-driven work.
3. Preserved prior project context as handoff entries.

Validation performed:
- Created both Markdown files.
- Packaged both files into a zip.

Known risks / follow-up:
- Agents must keep the log concise enough to remain useful.
- The log should be updated only for meaningful interactions, not every message.

Suggested commit:
```bash
git add AGENT_WORKFLOW.md AGENT_HANDOFF_LOG.md
git commit -m "docs: split agent workflow and handoff log"
```

## [2026-04-28] - UCRT third-party dependency staging and Intel RDFP wrapper hardening

Model / agent:
- GPT-5.5 Thinking, reasoning model

Source state:
- Latest available project snapshots and raw repo context from the SystemsTrading conversation.
- Direct `git clone` was not available in the execution container due to GitHub host resolution failure, so changes were prepared from uploaded/generated repo zips and raw file context.

User request:
- Fix local MinGW/UCRT third-party builds.
- Make the build script call the staging script.
- Make staging validate/download protobuf 29.3, Abseil, and Intel RDFP sources.
- Warn only for missing TWS API.
- Continue debugging Abseil, Intel RDFP, and final link failures.

Files changed:
- `scripts/stage_third_party_sources_ucrt.sh`
  - validates source trees under `third_party/`
  - downloads protobuf 29.3 with submodules into `third_party/_downloads/`
  - copies matching Abseil from protobuf submodule
  - downloads/extracts Intel RDFP
  - warns only for missing `third_party/twsapi/client`
  - supports `FORCE_RESTAGE_DEPS=1`
- `scripts/build_third_party_dependencies_ucrt.sh`
  - calls the staging script first
  - builds Abseil, protobuf, and Intel RDFP into `dependencies/ucrt64/install`
  - excludes `bid_binarydecimal.c` from the generated Intel RDFP wrapper
  - adds TWS BID compatibility wrappers for `__bid64_*`
  - uses a quoted here-document for generated CMake to prevent Bash expanding CMake variables
  - verifies required TWS decimal symbols with `nm`
- `CMakeLists.txt`
  - strengthens final executable linkage for IBKR builds
  - links protobuf, Intel RDFP, Threads or `ws2_32`, and `twsapi_vendor`
  - adds direct executable-level linkage to help MinGW static archive ordering
- `README.md`
  - documents UCRT compiler consistency and Intel RDFP wrapper behavior

Deletions / removals:
- No files removed.
- No project source files deleted.
- `bid_binarydecimal.c` is not deleted; it is excluded from the generated Intel RDFP wrapper target because it is optional for this use case and did not compile cleanly under MinGW/UCRT.

Steps taken:
1. Identified that `third_party/abseil-cpp` and protobuf-bundled Abseil could be malformed or stale.
2. Added staging logic to repair/download dependency source trees.
3. Hardened Abseil validation by checking `absl/base/config.h`, `absl/base/options.h`, and `ABSL_NAMESPACE_BEGIN/END`.
4. Excluded optional `bid_binarydecimal.c` from Intel RDFP wrapper build.
5. Added TWS-compatible `__bid64_*` wrapper symbols.
6. Fixed Bash here-doc escaping by using a quoted generated-CMake here-document.
7. Added symbol verification so missing BID symbols fail during dependency build instead of final app link.

Validation performed:
- Packaging and text generation were performed in the available environment.
- The user ran scripts locally and reported successive errors.
- Each reported error was used to update the next patch.

Known risks / follow-up:
- The TWS BID compatibility wrappers forward to Intel RDFP `bid64_*` functions; binary64 conversion wrappers use string conversion to avoid `bid_binarydecimal.c`.
- Final local validation should still be run on the user’s MinGW/UCRT machine.
- If additional `__bid*` symbols are needed by future TWS code paths, add wrappers and extend the `nm` verification list.

Suggested commit:
```bash
git commit -m "fix(ucrt): stage third-party deps and harden Intel RDFP wrapper"
```

## [2026-04-28] - Linux CI dependency release split

Model / agent:
- GPT-5.5 Thinking, reasoning model

Source state:
- Latest available project snapshots and raw GitHub workflow context.

User request:
- Build Linux dependencies once in CI and reuse them through release assets.
- Only force explicit dependency rebuilds when commit message contains `%REBUILD_DEPS`.

Files changed:
- `.github/workflows/ci.yml`
  - added/updated jobs for default build, coverage, Linux dependency release, and IBKR build
  - `linux-deps-release` gated by `%REBUILD_DEPS`
  - `ibkr-build` tries to download release asset and falls back to rebuilding/publishing if missing
- `scripts/rebuild_linux_deps_ci.sh`
  - builds Linux dependency bundle into `dependencies/linux/install`
  - archives `dependencies/linux/linux-deps-ubuntu-latest.tar.gz`
  - builds Abseil/protobuf and copies Intel decimal runtime assets
- `scripts/generate_ci_workflow.sh`
  - generates `.github/workflows/ci.yml` from a Bash here-document

Deletions / removals:
- No files removed.
- Malformed flattened workflow/script contents were replaced with valid multiline files.

Steps taken:
1. Identified producer/consumer split problem: dependency release job was conditional, but IBKR consumer was unconditional.
2. Added fallback in `ibkr-build`.
3. Added release asset upload/update path.
4. Fixed flattened YAML and shell script formatting.
5. Added `-DCMAKE_POSITION_INDEPENDENT_CODE=ON` for Linux Abseil/protobuf shared library compatibility.

Validation performed:
- Script/YAML structure was inspected.
- User ran CI and reported logs.
- Subsequent logs were used to patch PIC and formatting issues.

Known risks / follow-up:
- GitHub Actions release publishing requires `contents: write`.
- PRs and first-time runs depend on fallback rebuild path if release asset is unavailable.
- CI should be rechecked after pushing a valid multiline workflow.

Suggested commit:
```bash
git commit -m "ci: add reusable Linux dependency release bundle"
```

## [2026-04-28] - README rewrite

Model / agent:
- GPT-5.5 Thinking, reasoning model

Source state:
- Latest available README context from the SystemsTrading conversation.

User request:
- Redo the README.
- Remove old code-change notes.
- Add a short project description.
- Describe project organization, CI/local builds, scripts, CMake, expected locations, installed libraries, and release assets.

Files changed:
- `README.md`
  - rewritten as project documentation instead of accumulated change notes

Deletions / removals:
- Removed old change-log style README content.
- Removed old additive-only/code-change policy notes.
- Removed outdated IBKR SDK path notes.

Steps taken:
1. Replaced README with concise project overview.
2. Documented root layout, build targets, default build, IBKR build, UCRT flow, Linux CI bundle, release asset, scripts, CMake option, and runtime notes.

Validation performed:
- Markdown content was generated and provided as a downloadable file.

Known risks / follow-up:
- Keep README aligned with actual scripts as they evolve.

Suggested commit:
```bash
git commit -m "docs: rewrite project build and dependency README"
```
