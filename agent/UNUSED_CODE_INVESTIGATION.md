# UNUSED CODE INVESTIGATION — trading-system

Survey only; nothing was modified. Confidence key: **HIGH** = no callers anywhere in `src/`, `tests/`, or `include/` outside the defining file (and not linked from `CMakeLists.txt`); **MEDIUM** = the only references are self-references or human-only comments; **LOW** = active but appears superseded / redundant.

Scope excluded per instructions: `reports/runs/`, `data/`, `logs/`, `build*`, `cmake-build-*`, `dependencies/`, `third_party/`, `.git/`, `__pycache__/`, `.venv*/`, `tmp_*`, `*.log`. Also skipped shallow-search: `.claude/worktrees/*` (per-branch working copy).

---

## 1. Dead C++ Symbols

### HIGH confidence — no callers anywhere

| Symbol / definition | File:line | Reason |
|---|---|---|
| `struct MarketEvent` | `include/core/types.hpp:5` | Entire file's only content; the identifier appears in zero other files across `src/`, `tests/`, `include/`. |
| `class IEngine` (`step`, `~IEngine`) | `include/engine/IEngine.hpp:5` | No class inherits from it and no code creates an `IEngine*` / `IEngine&`. Grep for `public IEngine`, `: IEngine`, `IEngine*`, `IEngine::` returns zero hits outside the header itself. |
| `class SPSCQueue<T,N>` | `include/infra/spsc_queue.hpp:7` | Never included or instantiated. The live SPSC ring the logger uses is `hft::log::SpscRing` in `include/log/spsc_ring.hpp` (different, unrelated). |
| `class BufferedLogger` | `include/log/buffered_logger.hpp:8` | Header is never included; the identifier appears only inside this file. |
| `inline double compute_score(...)` | `include/models/score.hpp:7` | `RankingEngine.cpp:8` includes `models/score.hpp` but never calls `compute_score`; the live execution score is `compute_execution_score` from the different header `include/execution/score.hpp`. Include line itself is a stale leftover. |
| `int RankingEngine::top_k_` member | `include/engine/RankingEngine.hpp:19` | Initialized in ctor (`RankingEngine.cpp:17`), never read again. `live_top_k_` at line 17 above it holds the same value and is the one actually used at lines 131/133/134. |
| `double Stock::queue = 500.0` | `include/models/stock.hpp:24` | Assigned once (`LiveExecutionEngine.cpp:1631: s.queue = top.bid_size;`) but never read from anywhere. |
| `enum AppState::InitializingLogging` | `include/log/event_types.hpp:17` | Only reference is the `to_string` switch in `LoggingService.cpp:286`; nothing ever passes this value to `set_app_state`. |
| `enum AppState::ConnectingMarketData` | `include/log/event_types.hpp:18` | Same as above — only appears in the `to_string` switch (`LoggingService.cpp:288`). |
| `enum AppState::WaitingForServices` | `include/log/event_types.hpp:19` | Same — only in the `to_string` switch (`LoggingService.cpp:292`). |
| `enum AppState::Degraded` | `include/log/event_types.hpp:22` | Same — only in the `to_string` switch (`LoggingService.cpp:296`). |
| `enum AppState::RiskOff` | `include/log/event_types.hpp:23` | Same — only in the `to_string` switch (`LoggingService.cpp:298`). |
| `enum EventType::HealthSummary` | `include/log/event_types.hpp:55` | Never emitted; the `handle_event_` switch handles the case as a no-op (`LoggingService.cpp:245`) and the `to_string` switch mirrors it (line 362). `emit_health_summary_` is declared in the header but grep shows no definition. |
| `enum ComponentId::Persistence` | `include/log/event_types.hpp:42` | Never used as an argument anywhere; only in the `to_string` switch (`LoggingService.cpp:336`). |
| `enum ComponentId::Risk` | `include/log/event_types.hpp:40` | Never referenced except in `to_string`. |
| `enum ComponentId::Strategy` | `include/log/event_types.hpp:41` | Never referenced except in `to_string`. |
| `enum ComponentId::MarketData` | `include/log/event_types.hpp:39` | Only referenced from unit test `LoggingStateTest.cpp:100` (test-only). No production caller ever moves this component out of `Down`. |
| `void LoggingService::register_queue(EventQueue*)` | `include/log/logging_service.hpp:58`, `src/lib/LoggingService.cpp:75` | Grep for `register_queue(` returns only the definition — never called. `thread_queue()` self-manages via the internal `owned_queues_` vector. |
| `const std::vector<...> kLongGoalSymbolCompanyList` | `include/models/symbol_universe.hpp:9` | 115 entries, ~110 lines. Only reference outside its own definition is zero. Live path uses `kSymbolCompanyList` at line 117. |
| `RankingEngine::initialize(int)` overload's back-compat behaviour | `include/engine/RankingEngine.hpp:63`, `src/lib/RankingEngine.cpp:25` | The overload calling `initialize(kSymbolCompanyList, n_stocks)` is reachable, but only via `LiveExecutionEngine::initialize_universe` fallback when the file override is missing — worth confirming is truly wanted. LOW-adjacent HIGH. |

### MEDIUM confidence — only defined-and-self-referenced, unclear reader

| Symbol | File:line | Reason |
|---|---|---|
| `alias key "databento_download_mbp1_script"` in AppConfig parser | `src/lib/AppConfig.cpp:172` | Deprecated alias for `databento_l1_download_script`. Grep of every `.ini` (config/*, config.*.ini, and archived `reports/runs/*/config.ini`) shows zero uses of this key. Only kept as back-compat for pre-migration config files. |
| `alias key "databento_download_script"` | `src/lib/AppConfig.cpp:175` | Deprecated alias for `databento_l2_download_script`. Same story — no live config uses it. |
| `alias key "databento_dataset"` | `src/lib/AppConfig.cpp:179` | Deprecated alias for `databento_l2_dataset`. No live config uses it. |
| `alias key "databento_schema"` | `src/lib/AppConfig.cpp:183` | Deprecated alias for `databento_l2_schema`. No live config uses it. |

---

## 2. AppConfig fields — all live

I checked every field in `include/config/AppConfig.hpp`. Every field has at least one reader inside `src/lib/` and typically a test in `tests/unit/AppConfigTest.cpp`. **No dead config fields**. The four deprecated *aliases* above are the only cruft.

---

## 3. Test-only helpers unused by tests

### HIGH confidence

| Helper | File:line | Reason |
|---|---|---|
| `FakeIBKRTransport::request_market_data_type` override needed? | `tests/common/FakeIBKRTransport.hpp` | Note: this base-class default is `{}` and `FakeIBKRTransport` does NOT override it. That's fine. Nothing to flag here. |
| — | — | Grep of every helper in `tests/common/*.hpp` shows every method IS called by at least one test. `SimulatedOpenOrder`, `seed_open_orders`, `cancel_positions_count`, `SimulatedPosition`, `seed_positions` are all exercised (`TestBrokerIntegration.cpp:811-869`). `MockIBroker` / `MockIBKRTransport` methods all used from `LiveExecutionEngineTest.cpp` / `IBKRClientTest.cpp`. `TestFramework.hpp`'s `require`, `require_close`, `HFT_TEST` macros all live. |

Nothing dead in `tests/common/`.

---

## 4. Orphan files (never referenced / never compiled)

### HIGH confidence — code file not compiled and not included

| File | Why | Referenced-in-CMake? |
|---|---|---|
| `include/core/types.hpp` | Contains only `struct MarketEvent`; no other file `#include`s it. Header lives in the include path but is inert. | Header-only, not in CMake sources. **Never included.** |
| `include/engine/IEngine.hpp` | Contains only unused base class. | Not included. |
| `include/infra/spsc_queue.hpp` | Contains only unused `SPSCQueue`. | Not included. |
| `include/log/buffered_logger.hpp` | Contains only unused `BufferedLogger`. | Not included. |

### MEDIUM confidence — legacy / superseded script

| File | Why | Confidence |
|---|---|---|
| `scripts/build_vendored_ibkr.sh` | 10 lines wrapping `cmake .. && cmake --build`. No script or agent doc calls it (only mentioned once in AGENT_HANDOFF_LOG about dropping a flag from it). Superseded by CMake's auto-vendoring in the top-level `CMakeLists.txt`. | MEDIUM |
| `scripts/build_vendored_ibkr_with_protobuf.sh` | 25 lines, same shape; same status — no caller. | MEDIUM |
| `scripts/databento_download_mbp1.py` | Old L1 downloader. The AppConfig default L1 script is `local_l1_csv_provider.py`; no config `.ini` sets `databento_l1_download_script=scripts/databento_download_mbp1.py`. Only self-references (its own error message) and the AppConfig deprecated alias `databento_download_mbp1_script`. Superseded by `scripts/databento_download_l1.py` (whose CLI is called by `scripts/hft_l1_backfill.sh`). | MEDIUM |
| `scripts/run_hftbacktest_databento.py` | 24 KB, ~700 LOC. Purely-referenced in `agent/AGENT_HANDOFF_LOG.md` describing an old workflow. No live shell script or CI/systemd unit invokes it. Superseded by C++ `hft_app` + `DatabentoBacktestBroker`. | MEDIUM (deep-legacy) |
| `scripts/migrate_cache_layout.py` | Docstring says "Move legacy flat-layout L1/L2 caches into the dated per-window layout". One-shot migration script — grep shows zero callers anywhere. Since the layout migration already happened (existing caches are dated), it can likely go. | MEDIUM |
| `scripts/ibkr_historical_l1.py` | Only shell caller is `scripts/hft_l1_backfill.sh` which drives it in `--source ibkr` mode. The default `--source databento` uses `databento_download_l1.py`. It's still live but usage is a stale path — see `agent/AGENT_HANDOFF_LOG.md` describing the "future re-backfill" plan. Flag as LOW because the shell dispatch still calls it. | LOW |

### MEDIUM confidence — non-orphan but load-bearing question

| File | Why |
|---|---|
| `include/models/score.hpp` (`compute_score`) | Header is `#include`d by `src/lib/RankingEngine.cpp:8` but its `compute_score` symbol is never called there — stale include on a header that has no live users. |

---

## 5. Duplicate / superseded implementations

| Pair | File:line | Note |
|---|---|---|
| L1 downloader: `local_l1_csv_provider.py` (live, default) vs `databento_download_l1.py` (used by `hft_l1_backfill.sh`) vs `databento_download_mbp1.py` (dead) vs `ibkr_historical_l1.py` (only via `hft_l1_backfill.sh --source ibkr`) | `scripts/` | Three near-parallel L1 fetchers. `databento_download_mbp1.py` is definitely stale (`MEDIUM`); `ibkr_historical_l1.py` is a fallback path; `databento_download_l1.py` and `local_l1_csv_provider.py` differ (the latter reads a locally cached CSV). No hard duplicate — call graph is a Y, not a straight line. |
| Two "score" headers: `include/models/score.hpp` (`compute_score` — dead) vs `include/execution/score.hpp` (`compute_execution_score` — used) | `include/models/score.hpp`, `include/execution/score.hpp` | Classic old-and-new pair. `models/score.hpp` is a leftover. |
| `include/infra/spsc_queue.hpp` (`SPSCQueue<T,N>` — dead) vs `include/log/spsc_ring.hpp` (`SpscRing<T,Cap>` — live and load-bearing for logging) | `include/infra/spsc_queue.hpp`, `include/log/spsc_ring.hpp` | Two different, unrelated SPSC implementations. The `infra` one predates the logging refactor and has no reader. |
| No two DatabentoBacktestBroker or LocalSimBroker variants — one of each. Confirmed clean. |  | Just noted; not a finding. |

---

## 6. Unused Python — no clear dead functions inside live scripts

Scanned every top-level function in `scripts/*.py`:

- `scripts/check_branch_data.py` — live; called by `scripts/run_coverage_ci.sh:56`.
- `scripts/coverage_summary.py` — live; called by `scripts/run_coverage_ci.sh:62`.
- `scripts/compare_runs.py` — live CLI (documented in `agent/AGENT_WORKFLOW.md:168`). Note internal quirk: `main()` at line 120 contains `args = parse_args(argv) if False else __import__("argparse").Namespace()  # placeholder` — vestigial dead expression (`if False else`), immediately overwritten on line 122. **Cosmetic, not a dead symbol, but worth cleaning.**
- `scripts/warmup_engine.py` — live: docstring flags it as skeleton, but AppConfig / LiveExecutionEngine reference it and there's an active integration test path.
- `scripts/generate_html_report.py`, `plot_run.py`, `organize_runs.py`, `hft_monitor.py`, `hft_backtest_launcher.py`, `databento_l1_cost_quote.py`, `databento_download_l1.py`, `databento_download_l2.py`, `local_l1_csv_provider.py`, `ibkr_historical_l1.py`, `ibkr_symbol_contract_probe.py`, `scripts/backend/api.py` — all have live callers (systemd units, CI, `hft_backtest.sh`, or agent docs referring to operator invocation).

### HIGH-confidence dead Python scripts (whole files)

| File | Reason |
|---|---|
| `scripts/databento_download_mbp1.py` | Not called by any other script / CI / systemd unit / config; the "current" downloader is `databento_download_l1.py`. Only reference outside itself is the deprecated alias key `databento_download_mbp1_script` in `src/lib/AppConfig.cpp:172` (which itself has no live user). |
| `scripts/run_hftbacktest_databento.py` | Purely historical (Python-based hftbacktest scaffold). No shell/CI/systemd invocation. Superseded by `hft_app` + `DatabentoBacktestBroker`. |
| `scripts/migrate_cache_layout.py` | One-shot cache-format migration. Zero callers. Migration already completed. |

### `scripts/backend/api.py` — FastAPI endpoints

All endpoints (`/health`, `/runs`, `/runs/{run_id}`, `/live/status`, `/databento/credits`, `/backtests` GET+POST, `/backtests/{job_id}`, `/kill`, `/liquidate`, `/chat`) are documented as consumed by the mobile app (`agent/MOBILE_APP_DESIGN.md`). `/chat` returns 501 today (explicitly stubbed) — that's *incomplete*, not dead. Nothing to prune here.

---

## 7. Unused shell scripts

### HIGH confidence — no live caller

| Script | Reason |
|---|---|
| `scripts/build_vendored_ibkr.sh` | 10-line manual CMake wrapper. No CI job, no systemd unit, no `.md` doc invocation. Superseded by the top-level CMake's own IBKR / TWS API auto-vendor logic. |
| `scripts/build_vendored_ibkr_with_protobuf.sh` | Same pattern (25 lines with a "how to install protobuf" comment). Not called anywhere. |

### MEDIUM confidence — referenced only in docs

| Script | Reason |
|---|---|
| `scripts/hft_status.sh` | Only referenced in `agent/AGENT_HANDOFF_LOG.md` as "run manually to see what's on Hetzner". Not in CI, systemd, or another script. If the operator is using it interactively, keep; otherwise drop. |
| `scripts/format_code.sh` | Referenced in `agent/AGENT_WORKFLOW.md` as "run before staging C++ edits" (developer manual step). Not in CI (CI runs `check_clang_format.sh`). Legit developer tool — keep unless the workflow has changed. |

### Live scripts (verified)

`check_clang_format.sh`, `rebuild_linux_deps_ci.sh`, `smoke_app_ci.sh` — all invoked from `.github/workflows/ci.yml`.
`build_third_party_dependencies_ucrt.sh`, `stage_third_party_sources_ucrt.sh` — invoked from `agent/_run_deps_build.sh` and each other.
`hft_backtest.sh`, `hft_l1_backfill.sh`, `notify.sh`, `run_coverage_ci.sh`, `systemd/install.sh` — live.

---

## 8. `legacy/`, `old/`, `deprecated/`, `_old` directories

None found in the project tree (the only `legacy/` hit is inside `.venv-ibkr/Lib/site-packages/pip/…`, which is a virtualenv and explicitly out of scope). No `.bak` files.

---

## 9. Miscellaneous — Comments, `#if 0`, `[[deprecated]]`

Grepped `DEPRECATED`, `TODO.*DELETE`, `#if 0`, `[[deprecated]]`, `// XXX`, `// FIXME`, `// HACK`, `@deprecated` across `src/`, `tests/`, `include/`. **Zero hits** — the codebase is remarkably free of gating comments.

The only aged-marker pattern is the AppConfig parser's *two-name accepted key* trick (`key == "databento_l1_download_script" || key == "databento_download_mbp1_script"`) — see §1 MEDIUM findings and §6.

---

## 10. Summary of top actions (in priority order, no changes made)

1. **Delete four inert header-only files** (each contains only a symbol with zero users):
   - `include/core/types.hpp` (MarketEvent)
   - `include/engine/IEngine.hpp` (IEngine)
   - `include/infra/spsc_queue.hpp` (SPSCQueue)
   - `include/log/buffered_logger.hpp` (BufferedLogger)
2. **Delete `include/models/score.hpp`** and remove the stale `#include "models/score.hpp"` at `src/lib/RankingEngine.cpp:8`.
3. **Drop unused enum values** from `include/log/event_types.hpp`: `AppState::InitializingLogging`, `ConnectingMarketData`, `WaitingForServices`, `Degraded`, `RiskOff`; `EventType::HealthSummary`; `ComponentId::Persistence`, `Risk`, `Strategy` (and possibly `MarketData` — test-only reader). Update the corresponding `to_string` switches in `src/lib/LoggingService.cpp`.
4. **Drop `LoggingService::register_queue`** — never called; `thread_queue()` self-manages queues.
5. **Drop `RankingEngine::top_k_`** and its initializer — `live_top_k_` is the real field.
6. **Drop `Stock::queue`** — written once, read zero times.
7. **Drop `kLongGoalSymbolCompanyList`** (~110 lines) from `include/models/symbol_universe.hpp`.
8. **Delete three dead Python scripts**: `scripts/databento_download_mbp1.py`, `scripts/run_hftbacktest_databento.py`, `scripts/migrate_cache_layout.py`.
9. **Delete two dead shell scripts**: `scripts/build_vendored_ibkr.sh`, `scripts/build_vendored_ibkr_with_protobuf.sh`.
10. **Consider dropping** the four deprecated AppConfig alias keys (`databento_download_mbp1_script`, `databento_download_script`, `databento_dataset`, `databento_schema`) — no live `.ini` uses them.
11. **Cosmetic**: fix `scripts/compare_runs.py:120`'s vestigial `args = parse_args(argv) if False else …` placeholder.

---

## Could-not-verify list

None. Every grep and read completed successfully on this session; no network / rate-limit failures were encountered.
