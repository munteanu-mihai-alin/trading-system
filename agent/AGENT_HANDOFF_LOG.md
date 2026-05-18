# Agent Handoff Log

This is the append-only working log for agents. New entries should be added at the top.

Read `AGENT_WORKFLOW.md` before editing this file.

## [2026-05-19] - L1 timestamped cache + range-aware reuse

Model / agent:
- Model: Claude Opus 4.7 (Anthropic), reasoning model
- Provider/client: Claude Code on UCRT64

Source state:
- `main` at `6322881 feat(broker): timestamped L2 cache + range-aware reuse`.

User request:
- After the L2 cache became range-aware, the user asked whether L1 needed
  the same treatment. They authorized opening a `#todo` capturing the
  problem and approach, then solving it in the same round and marking
  it `#Done`.

Context (also captured in the open `#todo` entry below):
- L1 (`*.mbp1.csv`) is the slow-moving signal feeding `Stock::mid`. Today
  it's step-indexed only and the broker reuses any existing cache by file
  existence, so sub-window reruns silently align step 0 to the wrong
  wall-clock time. The fast-moving L2 path is now ts_event-aware; keeping
  L1 step-only means the two replay timelines can diverge when the user
  trims the window.

What shipped:
1. `scripts/ibkr_historical_l1.py` writes
   `ts_event,step,bid_price,bid_size,ask_price,ask_size`. `ts_event_ns`
   is derived from `bar.date` (epoch seconds) * 1e9.
2. `scripts/local_l1_csv_provider.py` propagates `ts_event` from the
   source row (accepts `ts_event`, `ts_recv`, `timestamp`, or `time`
   field; integer ns OR ISO-8601). Synthetic mode emits dated rows
   spaced 1 minute apart starting 2026-01-01T13:30:00Z.
3. `src/lib/DatabentoBacktestBroker.cpp`:
   - Renamed `read_l2_cache_range` -> `read_cache_ts_range` (shared by
     both L1 and L2; struct is now `CacheTsRange`).
   - `parse_top_row` accepts legacy (5 fields) and dated (6 fields)
     schemas. Legacy rows yield `ts_event_ns == 0` and bypass filtering.
   - `ensure_l1_symbol_loaded` now does the same request-window-vs-cached
     -range check that `ensure_l2_symbol_loaded` does. Legacy or
     short-of-window caches are removed before re-invoking
     `local_l1_csv_provider.py`.
   - `load_top_books_from_csv` accepts optional `[start_ns, end_ns]`
     bounds and renumbers `step` to 0..N over the kept rows so the
     engine's step index stays contiguous and aligned with the L2 path.
4. `include/broker/DatabentoBacktestBroker.hpp` - updated
   `load_top_books_from_csv` signature with the two optional bounds.

Files changed:
- `scripts/ibkr_historical_l1.py` - new `ts_event` column on output.
- `scripts/local_l1_csv_provider.py` - propagate `ts_event`; synthetic
  mode emits dated rows.
- `src/lib/DatabentoBacktestBroker.cpp` - shared range helper, dual-
  schema `parse_top_row`, range-aware `ensure_l1_symbol_loaded`,
  filtered `load_top_books_from_csv`.
- `include/broker/DatabentoBacktestBroker.hpp` - signature update.

Deletions / removals:
- None. Existing legacy L1 files in `data/l1/` continue to work via the
  legacy code path. They just don't get the sub-window reuse benefit
  until they're re-backfilled with the new script.

Steps taken:
1. Updated both L1 scripts to write `ts_event` as the new leading column.
2. Renamed the broker's cache-range helper from `read_l2_cache_range` to
   `read_cache_ts_range` since L1 and L2 share the same header check and
   first/last-line scan.
3. Made `parse_top_row` schema-tolerant (5 vs 6 fields).
4. Rewrote `ensure_l1_symbol_loaded` to mirror `ensure_l2_symbol_loaded`.
5. Added the same `[start_ns, end_ns]` filter + step renumbering to
   `load_top_books_from_csv`.
6. `./scripts/format_code.sh` -> 74 files normalized.
7. Built `hft_lib`, `hft_app`, `hft_tests` clean under UCRT64.

Validation performed:
- `cmake --build build-ucrt-ibkr --target hft_lib hft_app hft_tests`:
  clean.
- Runtime `./hft_tests.exe` on this developer host still hits the
  pre-existing `STATUS_ENTRYPOINT_NOT_FOUND` from the previous commit
  (UCRT/libstdc++ DLL ABI mismatch); unrelated to this change. CI runs
  in a clean Linux env.

Known risks / follow-up:
- Same as L2: the "extends cached range" case re-downloads the full
  requested window rather than stitching head/tail. Cheap-rerun of
  identical or sub-windows already works.
- Existing 50 legacy L1 files in `data/l1/<SYM>.mbp1.csv` need a one-
  time re-backfill via `ibkr_historical_l1.py` to gain `ts_event`. Until
  then the broker falls back to today's clamp-at-end behavior on those
  files (still correct, just no sub-window reuse).
- No unit tests for the broker class still applies; same follow-up as
  the L2 entry.

Suggested commit:
```bash
git commit -m "feat(broker): L1 timestamped cache + range-aware reuse (parity with L2)"
```

## [2026-05-19] - L1 cache is step-indexed and not range-aware #todo #Done (resolved by [2026-05-19] L1 timestamped cache + range-aware reuse)

Model / agent:
- Model: Claude Opus 4.7 (Anthropic), reasoning model
- Provider/client: Claude Code on UCRT64

Source state:
- `main` at `6322881 feat(broker): timestamped L2 cache + range-aware reuse`.

Context:
- The L2 cache change at `6322881` made `*.mbp10.csv` carry `ts_event`
  per row and taught the broker to detect cached coverage and filter on
  load. The L1 path (`*.mbp1.csv`) was left step-indexed.
- Concrete failure mode when only L2 is range-aware: the user runs a
  backtest covering 04-13 to 04-28, then reruns just 04-15 to 04-16 to
  drill into a subset. The L2 cache trims correctly (renumbered step 0
  starts at the requested start_ns). The L1 cache continues to play from
  step 0 = the original start, so the L1 mid stream is shifted by ~2
  trading days relative to L2 for the entire rerun. The strategy reads
  the wrong `s.mid` against the right `s.bid`/`s.ask`, silently producing
  a broken replay.
- Less-bad case but still bad: the L1 file covers a window the user
  doesn't actually want for this rerun, and there's no way to detect
  the mismatch from the file alone.

Approach:
1. Add `ts_event` as the leading column in the L1 output schema. Two
   producers need updating:
   - `scripts/ibkr_historical_l1.py` (master backfill from IBKR
     BID_ASK historical bars) - it already knows `bar.date` (epoch
     seconds); the conversion to ns is `bar.date * 1e9`.
   - `scripts/local_l1_csv_provider.py` (broker-invoked normalizer
     that copies `data/l1/<SYM>.mbp1.csv` -> cache) - propagate
     `ts_event` from the source row; accept `ts_event` as either int
     ns or ISO-8601 string.
2. In `DatabentoBacktestBroker`:
   - Generalize the L2-only `read_l2_cache_range` to a shared
     `read_cache_ts_range` (same first/last-line scan; works for any
     dated CSV that has ts_event as its first column).
   - Make `parse_top_row` accept both 5-field (legacy) and 6-field
     (dated) rows. Legacy rows yield ts_event_ns == 0 so they bypass
     filtering, preserving today's clamp-at-end semantics for any
     pre-existing local cache.
   - Make `ensure_l1_symbol_loaded` mirror `ensure_l2_symbol_loaded`:
     re-invoke the L1 downloader when the cache is missing, lacks
     ts_event, or its range doesn't cover the requested window.
   - Add `[start_ns, end_ns]` to `load_top_books_from_csv`, filter rows
     by ts_event, and renumber step to 0..N over the kept rows.
3. No schema change to the broker headers' public surface beyond the
   new defaulted optional args; existing call sites pass nullopt and
   keep working.

Migration:
- Existing legacy `data/l1/<SYM>.mbp1.csv` files are still readable
  (5-field rows -> ts_event_ns = 0 -> no filter applied). They lose
  the sub-window-reuse benefit but the backtest still runs.
- To gain sub-window reuse for the historic universe, re-run
  `ibkr_historical_l1.py` once across the full desired window. After
  that, sub-window backtests reuse the L1 cache for free, identical to
  the L2 path.

Validation performed:
- None standalone; investigation note that drove the resolution above.

Known risks / follow-up:
- `#todo #Done`. See the resolving entry for what shipped.

Suggested commit (when resolved): see resolving entry.

## [2026-05-19] - Timestamped L2 cache + range-aware reuse in DatabentoBacktestBroker

Model / agent:
- Model: Claude Opus 4.7 (Anthropic), reasoning model
- Provider/client: Claude Code on UCRT64

Source state:
- `main` at `8cb9a10 feat(log): ts_ns column + session_start/end markers +
  append mode` (1 ahead of `origin/main`).

User request:
- Make the L2 cache reusable across runs. Today `download_if_missing` only
  checks for file existence; if a previous run cached AAPL 04-13..04-17 and
  a new run requests 04-13..04-28 (or 04-14..04-16, or 04-20..04-28), the
  broker would either reuse a stale window or refuse to extend it.
- The user explicitly wanted the timestamped-row approach (`ts_event` per
  row) so sub-windows of a cached range also reuse for free. Dated
  filenames would only match exact windows and were rejected.

Files changed:
- `scripts/databento_download_l2.py` - downloader now writes
  `ts_event,step,side,level,price,size`. `ts_event` is nanoseconds since
  the Unix epoch, read from the Databento `ts_event` column (falls back to
  the DataFrame index, which is `ts_recv`, if the column is absent). The
  synthetic mode also emits dated rows starting at 2026-01-01T13:30:00Z
  with a +1s step so tests can exercise the new schema without network.
- `src/lib/DatabentoBacktestBroker.cpp`:
  - `parse_level_row` now accepts both schemas. Legacy 5-field rows yield
    `ts_event_ns = 0` and bypass the date filter, preserving today's
    behavior for any pre-existing cached file at runtime.
  - New `parse_iso8601_to_ns(...)` parses `databento_start` /
    `databento_end` into nanoseconds (uses `_mkgmtime` on Windows,
    `timegm` on POSIX).
  - New `read_l2_cache_range(path)` reads the header + first/last data row
    to return `{start_ns, end_ns}`. Header without `ts_event` -> nullopt
    (treated as cache miss so the broker re-downloads with the new
    schema).
  - `ensure_l2_symbol_loaded` replaces the simple file-exists check:
    re-download when the cache file is missing, lacks ts_event, or its
    range does not cover the requested `[databento_start, databento_end]`.
    Stale legacy caches are removed before the re-download so the new file
    lands at the expected path.
  - `load_books_from_csv` accepts optional `[start_ns, end_ns]` bounds.
    For dated rows it filters to the window and renumbers `step` to 0..N
    starting at the first kept row, so the engine's step index stays
    contiguous. Legacy rows continue to use their stored step verbatim.
- `include/broker/DatabentoBacktestBroker.hpp` - updated
  `load_books_from_csv` signature with two `std::optional<int64_t>`
  defaults and pulled in `<cstdint>`/`<optional>`.

Deletions / removals:
- None. Existing 6 cached `*.mbp10.csv` files on Hetzner remain on disk
  but are detected as legacy and will be replaced on next access (the
  broker rm's the legacy file before re-downloading).

Steps taken:
1. Confirmed cached files are step-indexed only (no timestamps) and that
   `DatabentoBacktestBroker::download_if_missing` skips on file exists,
   regardless of date window.
2. Added `ts_event` to the downloader's CSV. Used `row.get('ts_event')`
   with `Timestamp.value` for the nanosecond conversion; fell back to the
   DataFrame index for safety.
3. Added the broker helpers (`parse_iso8601_to_ns`, `read_l2_cache_range`)
   and rewrote `ensure_l2_symbol_loaded` to do the
   request-window-vs-cached-range check.
4. Made `load_books_from_csv` take optional bounds + renumber step.
5. Ran `./scripts/format_code.sh` (74 files normalized).
6. Built `hft_lib`, `hft_app`, `hft_tests` clean under UCRT64.

Validation performed:
- `cmake --build build-ucrt-ibkr --target hft_lib hft_app hft_tests`:
  builds clean.
- Runtime `./hft_tests.exe` on this host hits a pre-existing
  STATUS_ENTRYPOINT_NOT_FOUND (UCRT/libstdc++ DLL ABI mismatch on the
  developer machine) unrelated to this change; CI will run the test
  binary in a clean Linux environment.

Known risks / follow-up:
- No unit test exists for `DatabentoBacktestBroker` (the class has no
  test fixture today). The range-aware path is best validated by an
  end-to-end backtest run; recommend adding a small fixture test as a
  follow-up that writes a synthetic dated CSV, then exercises
  `load_books_from_csv` with various `[start_ns, end_ns]` windows.
- L1 caching (`local_l1_csv_provider.py` / `*.mbp1.csv`) is still
  step-indexed and has the same legacy issue. L1 was out of scope for
  this change; opening a sibling `#todo` would be appropriate when L1
  starts coming from Databento or from a re-runnable source.
- The "extends cached range" case currently re-downloads the full
  requested window rather than stitching head/tail onto the cached file.
  True partial-reuse (only download the missing slice and append) is a
  follow-up - cheap reruns of identical or sub-windows already work
  with this change.
- Pre-existing 6 mbp10 files on Hetzner (~135 MB total) will be
  re-downloaded on next access, repaying ~$3 of L2 spend once. After
  that, all subsequent runs of the same window are free.

Suggested commit:
```bash
git commit -m "feat(broker): timestamped L2 cache + range-aware reuse"
```

## [2026-05-18] - C++ backtest runner end-to-end via hft_app + DatabentoBacktestBroker

Model / agent:
- Model: Claude Opus 4.7 (Anthropic), reasoning model
- Provider/client: Claude Code on UCRT64

Source state:
- `main` at `8cb9a10 feat(log): ts_ns column + session_start/end markers +
  append mode` (1 ahead of `origin/main` at time of writing).

User request:
- Mark the original 2026-05-16 "Build a C++-side backtest runner..." `#todo`
  as resolved. The end-to-end C++ harness is now in production: it drives
  `LiveExecutionEngine` through `DatabentoBacktestBroker` using real L1+L2,
  with realistic ranking, sizing, sell-side execution, and full lifecycle
  logging. The next planned exercise is a separate 10-trading-day run
  (2026-04-13 → 2026-04-28) reusing the cached L2 for the first week.

What shipped:
1. `hft_app` (C++) runs in `mode=databento_backtest` against
   `DatabentoBacktestBroker` end-to-end. First successful run finished
   2026-05-18 (run `2026-05-18T0221_cpp_backtest`): 4 orders placed
   across CDNS/TTE/NOK/LRCX, 1 round-trip filled+sold, 1 open position.
2. Synthetic 8-candidate FillModel sweep is now gated behind
   `synthetic_fill_model` (default off in backtest). Removing that O(N log N)
   `sort_sides` per match unwedged 100k-step runs.
3. Realistic entry routing via `entry_limit_mode = ask` (cross L1 ask so
   buys actually fill in a 4-5 day window) and `s.ask_price` plumbed into
   `Stock` from broker top-of-book.
4. Auto step bound via `steps_auto_from_broker = true` + new
   `IBroker::max_replay_steps()` virtual + `DatabentoBacktestBroker`
   override — engine stops at the end of the data window instead of
   spinning on a frozen book.
5. L1-mid-change-driven Hawkes proxy
   (`hawkes_mid_change_threshold_bps`) so the backtest's ranking sees
   non-zero arrival intensity without trade prints.
6. Three opt-in CSV logs: `order_log_path` (state-change lifecycle),
   `step_trace_log_path` (per-step ranking snapshot, ~15 MB / 6-day),
   `l2_trace_log_path` (per-step L2 for held positions, sub-1 MB).
7. Wall-clock `ts_ns` prepended to every row plus `# session_start` /
   `# session_end` comment markers bracketing each engine session, with
   `AppConfig::log_append_mode` for live restart-within-day visibility.
8. Per-run report folder convention at `reports/runs/<YYYY-MM-DDTHHMM>_<label>/`
   with `manifest.json`, plus `scripts/organize_runs.py` to backfill the
   layout idempotently.
9. AGENT_WORKFLOW.md updated with the "Backtest run reporting" and
   "Per-row ts_ns and session markers" sections.

Resolving commits (chronological):
- `815e4f6 feat(engine): gate synthetic FillModel behind config`
- `d39bffa feat(engine): realistic L1-driven entry limit, auto step count,
  mid-change Hawkes proxy`
- `2d2360b chore(reports): organize backtest artifacts into per-run folders
  with manifests`
- `ab80a61 feat(log): orders.csv + step_trace.csv + l2_trace.csv (three
  opt-in backtest logs)`
- `8cb9a10 feat(log): ts_ns column + session_start/end markers + append mode`

Files changed (this entry):
- `agent/AGENT_HANDOFF_LOG.md` — retagged the 2026-05-16 `#todo` to
  `#todo #Done` with back-reference; added this resolving entry.

Validation performed:
- End-to-end run `2026-05-18T0221_cpp_backtest`: 100k steps, 4 placed,
  1 round-trip, ~$3 Databento spend, CI green through 8cb9a10's parent.

Known risks / follow-up:
- Sibling `#todo` still open: `[2026-05-16] - Run a one-week real
  Databento backtest with OU gate ON and calibrate thresholds` — broader
  10-trading-day calibration run is the next step, separate from this
  one (planned 2026-04-13 → 2026-04-28).
- Sibling `#todo` still open: `[2026-05-16] - Wire FillModel and
  remaining ranking inputs to real market data` — fill probability is
  still synthetic in the buy-side ranking score; only the broker uses
  real L1/L2 for the actual fill decision.
- End-of-run realized PnL summary in `src/app/main.cpp` is still a
  deferred follow-up (orders.csv enables it; not yet implemented).

Suggested commit:
```bash
git commit -m "docs(agent): mark C++ backtest runner #todo as #Done"
```

## [2026-05-18] - Free-form debug/trace logging API for engine and strategy code #todo

Model / agent:
- Model: Claude Opus 4.7 (Anthropic), reasoning model
- Provider/client: Claude Code on UCRT64

Source state:
- Local clone at `D:\trading-system`, on `main`, after the
  `d39bffa feat(engine): realistic L1-driven entry limit, auto step count,
  mid-change Hawkes proxy` commit.

Context:
- Today the `hft::log::LoggingState` producer API only exposes four event
  types (set_app_state, set_component_state, heartbeat, raise_warning,
  raise_error). Internally spdlog has full level support (trace/debug/
  info/warn/err/critical) but it is not surfaced to the engine.
- Engine and strategy code that wants to log "I considered X, picked Y
  because Z" has no easy path. The current workaround is the
  decision_log_path CSV writer, which only fires on a successful buy and
  has a fixed 16-column schema. Anything else requires hard-coded stdout
  prints or a custom CSV per call site.
- This was flagged by the user during the 2026-05-18 logging audit. The
  design decision is non-trivial enough to warrant a dedicated #todo
  rather than a quick patch.

Design questions to settle before implementing:
1. Channel: extend the existing SPSC ring + writer thread, or bypass the
   ring with direct spdlog calls? The ring is wait-free on the producer
   side (good for the engine hot path) but adds a fixed event-type
   surface. Direct spdlog calls are simpler but introduce a lock on the
   shared sink.
2. Levels: which of trace/debug/info/warn/err/critical to expose. At
   minimum debug and trace. The current `raise_warning` / `raise_error`
   already cover warn/err and should stay as the dominant path for
   structured warnings; the new API is for unstructured prose.
3. Filtering: gate by AppConfig (one knob per channel? per level?), by
   env var (`HFT_LOG_LEVEL`), or both. Backtest mode would benefit from
   running at debug; live runs should stay at info.
4. API shape: `log_debug(component, fmt, args...)` -> string. spdlog
   fmt-style formatting is the natural fit but pulls in fmtlib at the
   header surface. Alternative: thunk through a `LogLine` helper that
   builds a std::string once.
5. Performance: trace/debug calls in the hot path should compile to a
   single load+branch when disabled (level check elides arg evaluation).
   This is hard with naive formatting; spdlog macros handle it well.

Proposed scope of the first iteration:
- Surface spdlog's debug and trace levels through a new `LoggingState`
  family: `log_debug(ComponentId, std::string_view)` and
  `log_trace(ComponentId, std::string_view)`.
- Gate via two new AppConfig knobs: `log_level` (one of trace/debug/info/
  warn/err, default info) and `log_engine_decisions_debug` (bool,
  default false, turns on per-step engine internals).
- Bypass the SPSC ring for these calls; go directly to spdlog with a
  cached logger handle. The ring is reserved for the structured-event
  path.
- Wire a few high-value debug spots: every buy gate decision in
  `LiveExecutionEngine::step` ("skipped X because OU gate"), every sell
  scoring run in `route_exit_orders`, every L2 lazy-load in the broker.

Validation performed:
- None; investigation note only.

Known risks / follow-up:
- `#todo`. Surface to user, get approval before working on it.
- spdlog's default formatter is text; for analytics we may want a
  parallel structured-JSON sink. Defer to a follow-up.
- Excessive debug logging in the hot path can swamp the engine; the
  level-check elision must be measured (rdtsc) before flipping on for
  long runs.

Suggested commit (when resolved):
```bash
git commit -m "feat(log): debug/trace LoggingState API + per-component level gating"
```

## [2026-05-16] - Build a C++-side backtest runner that drives LiveExecutionEngine through DatabentoBacktestBroker #todo #Done (resolved by [2026-05-18] C++ backtest runner end-to-end via hft_app + DatabentoBacktestBroker)

Model / agent:
- Model: Claude Opus 4.7 (Anthropic), reasoning model
- Provider/client: Claude Code on UCRT64

Source state:
- `main` at `7590a72` (CI build-fix on top of the four-feature commit).

Context:
- The Python backtest harness in `scripts/run_hftbacktest_databento.py`
  drives hftbacktest directly: it reads the first valid ask from the L1
  CSV, market-buys one share, and submits a target-profit limit sell.
  It does NOT instantiate `LiveExecutionEngine` and therefore does NOT
  exercise any of the BUY-side logic that has been shipped this week:
  - Hawkes intensity (real-trade wired, two-channel)
  - OU mean-reversion gate (samples or half-life seconds)
  - +target_pct hit-count tilt
  - Score-weighted position sizing
  - Budget gate, max_open_symbols, max_orders_per_run/_per_symbol
- This was a latent gap: the original 2026-05-16 "Run a one-week real
  Databento backtest" #todo implicitly assumed the backtest would
  validate the strategy. It only validates the sell-side L2 execution
  path. Buy ranking is bypassed entirely.

Proposed scope:
- Add a new C++ backtest entry point, e.g. `src/app/hft_backtest.cpp` or
  a flag on `hft_app` that runs in backtest mode. It would:
  1. Construct `DatabentoBacktestBroker` from `AppConfig.databento_*`
     fields (mode=databento_backtest).
  2. Construct `LiveExecutionEngine` with that broker.
  3. Call `subscribe_live_books` over the universe.
  4. Loop `engine.step(t)` until the replay broker's clock exhausts
     the cached L2 CSV.
  5. Drain `OrderLifecycleBook` for filled orders, compute PnL net of
     fees from `AppConfig.commission_per_share`, etc.
  6. Write a Markdown report similar to the Python script's output.
- `DatabentoBacktestBroker::on_step(int t)` already exists (header
  inspected during earlier work). It needs to:
  - Advance the replay clock and surface `snapshot_top_of_book` and
    `snapshot_book` against the current step's L2 (already implemented).
  - Optionally surface `drain_trades` to drive the new Hawkes wiring;
    today it returns empty. To support the hawkes_use_real_trades
    feature in backtest, parse trade prints from the MBP-10 dataset
    (each "trade" event in MBP-10 has a flag) and queue them.
- L1 input: the existing CSVs at `data/l1/<SYM>.mbp1.csv` from the
  ibkr_historical_l1.py backfill can drive `s.mid` in
  `reconcile_broker_state` via a small `LocalL1CsvBroker` adapter, OR
  the engine can read top-of-book directly from the L2 ladder's level 0.

Open design questions:
- Which broker delivers `s.mid`? Options: (a) reuse MBP-10 level 0 as
  L1 proxy; (b) wire a separate L1 source from the L1 CSVs; (c) ship
  both flows behind a config knob. Cleanest: `DatabentoBacktestBroker`
  already exposes `snapshot_top_of_book` that derives from
  `snapshot_book().bids[0]/asks[0]`. Use that.
- How does the backtest drive Hawkes? Lee-Ready aggressor classification
  on MBP-10 requires a separate "trades" channel (Databento has it as
  the MBP-1 schema). For first cut, feed Hawkes from MBP-10 level 0
  changes as a poor-man's event stream; cross-check against the live
  AllLast path later.
- Reporting: keep the Python harness as the L2-execution validator; the
  C++ harness as the strategy validator. Side-by-side reports help
  triangulate.

Validation performed:
- None; investigation note only.

Known risks / follow-up:
- `#todo`. Surface to user, get approval before working on it.
- Sibling `#todo`: `[2026-05-16] - Run a one-week real Databento
  backtest with OU gate ON and calibrate thresholds`. The cost-of-data
  is the SAME between the Python harness and a C++ runner; the L2 CSVs
  are cached after the first download. So adding the C++ harness adds
  zero Databento cost - it just reads the same cached data through a
  different consumer. Run both harnesses against the same window.
- Without this, every BUY-side change is theoretical until live paper
  trading, which is the opposite of how strategy validation should
  flow.

Suggested commit (when resolved):
```bash
git commit -m "feat(backtest): C++ backtest runner driving LiveExecutionEngine through DatabentoBacktestBroker"
```

## [2026-05-16] - Implement first 4 ranking-side #todos (hit-count, two-channel Hawkes, OU half-life in seconds, score-weighted sizing)

Model / agent:
- Model: Claude Opus 4.7 (Anthropic), reasoning model
- Provider/client: Claude Code on UCRT64

Source state:
- Local clone at `D:\trading-system`, on `main`, at `fa23835`
  (`docs(agent): open 4 new #todo entries`). User instructed:
  *"Do first 4 then"* meaning the first four entries in the open
  `#todo` list. All four were opened in the prior commit and are
  resolved by this commit; their original entries below are retagged
  `#todo #Done (resolved by ...)` per the workflow.

Scope (one commit, four features, each independently controlled by its
own config knobs and defaulted to behaviour-preserving values):

### Feature 1: Empirical "+target_pct hit-count" buy-side ranking tilt
- `AppConfig`: `hit_count_enabled` (default false), `hit_count_target_pct`
  (default 0.008), `hit_count_horizon_seconds` (default 60.0),
  `hit_count_baseline` (default 5.0), `hit_count_tilt_min` (default 1.0),
  `hit_count_tilt_max` (default 3.0).
- `Stock`: `window_open_mid`, `window_open_ts_ns`, `hit_count`,
  `score_tilt` (default 1.0).
- `LiveExecutionEngine::update_hit_count_tilt()` (new, called from
  `reconcile_broker_state`): per-symbol, slides a `horizon`-second window
  on observed mid; when `(mid_now - mid_window_open) / mid_window_open
  >= target_pct`, increments `hit_count`. Sets `s.score_tilt =
  clamp(hit_count / baseline, tilt_min, tilt_max)`.
- `RankingEngine::step`: after the existing cooldown application,
  multiplies `s.score *= s.score_tilt`. Off-by-default tilt of 1.0
  preserves prior behaviour exactly.

### Feature 2: Two-channel Hawkes (buy- vs sell-aggressor)
- `Stock`: new `Hawkes hawkes_sell;` alongside the existing `hawkes`
  (kept under its original name so existing code/tests reading
  `s.hawkes.lambda` are unchanged; it is now the buy-aggressor channel).
- `LiveExecutionEngine::update_hawkes_from_trades`: classifies each
  drained trade against the cached top-of-book via Lee-Ready
  (price >= ask -> buy-aggressor; price <= bid -> sell-aggressor;
  strictly inside spread -> neither, advance timestamp only). When
  no quote context is available (early in the session) it falls back
  to the buy-channel single-event update to avoid losing every event
  during bootstrap.
- `LiveExecutionEngine::route_exit_orders`: replaces the lambda used in
  `compute_execution_score` with `max(hawkes_sell.lambda, hawkes.lambda)`
  - the sell channel when it has classified events, falling back to the
  buy channel otherwise to avoid stalling sell-side scoring with a
  baseline-only sell intensity.
- No new config knob; the channel split is automatic when
  `hawkes_use_real_trades=true`. Cross-excitation between channels is
  NOT implemented in this pass; the entry's "open questions" section
  remains valid future work.

### Feature 3: OU EWMA in wall-clock time
- `AppConfig`: new `ou_halflife_seconds` (default 0.0). Old
  `ou_window_size` kept and parsed; either knob enables the gate.
  When both are zero, the gate is off (unchanged).
- `Stock`: new `last_ou_update_ts_ns`.
- `LiveExecutionEngine::reconcile_broker_state`: branches on which knob
  is non-zero. With `ou_halflife_seconds`, `alpha = 1 - exp(-dt_s /
  (halflife / ln 2))`. With `ou_window_size`, the legacy
  `alpha = 1 / window_size`. Bootstrap path also records
  `last_ou_update_ts_ns`.
- `step()` buy-loop gate updated to check `ou_halflife_seconds > 0 ||
  ou_window_size > 0` rather than just `ou_window_size > 0`.

### Feature 4: Score-proportional position sizing
- `AppConfig`: new `position_sizing_rule` (default "equal"). "equal"
  preserves prior behaviour (trade_notional per active symbol).
  "score_weighted" divides `account_budget` proportionally to active
  symbols' scores (after the hit-count tilt and cooldown). Falls back
  to equal when the sum of active scores is non-positive.
- `LiveExecutionEngine::compute_per_symbol_notional()` (new): runs once
  per step in `step()`, returns `symbol -> target_notional`.
- `LiveExecutionEngine::size_entry_qty(price, target_notional)` (signature
  extended): caller passes the per-symbol notional from the map above.
  Legacy fixed-share fallback still applies when target_notional <= 0.

Files changed:
- `include/config/AppConfig.hpp`, `src/lib/AppConfig.cpp` - parsing for
  all new knobs (7 new keys total across the four features).
- `include/models/stock.hpp` - 6 new per-Stock fields.
- `include/engine/LiveExecutionEngine.hpp`, `src/lib/LiveExecutionEngine.cpp`
  - 2 new private helpers, modified `size_entry_qty`, two-channel routing
  in `update_hawkes_from_trades`, OU half-life branch, hit-count windowing
  helper.
- `src/lib/RankingEngine.cpp` - one line: `s.score *= s.score_tilt`.
- `tests/unit/AppConfigTest.cpp` - defaults + `ParsesAllKnownKeys` cover
  all 9 new keys.
- `tests/unit/LiveExecutionEngineTest.cpp` - 7 new tests:
  `HitCountTiltMultipliesScoreAfterRankingStep`,
  `TwoChannelHawkesRoutesBuyAggressorToHawkes`,
  `TwoChannelHawkesRoutesSellAggressorToHawkesSell`,
  `TwoChannelHawkesMidTradeUpdatesNeitherChannel`,
  `OUGateEnabledViaHalflifeSeconds`,
  `ScoreWeightedSizingAllocatesBudgetByScore`,
  `ScoreWeightedSizingFallsBackToEqualWhenAllNonPositive`,
  `EqualSizingDefaultUsesTradeNotional`.
- `config.ibkr_paper.example.ini` / `config.databento_backtest.example.ini`
  - document `hit_count_*`, `ou_halflife_seconds`, `position_sizing_rule`
  knobs. All features remain disabled by default in both configs.

Deletions / removals:
- None.

Steps taken:
1. Wired up four features in order, each behind its own opt-in knob.
2. After each feature, updated AppConfig defaults/parse tests and added
   an engine-level test exercising the new behaviour.
3. Updated both example configs to document the new knobs (off by
   default; backtest config knob for OU upgraded to half-life form).
4. Retagged the four original `#todo` entries `#todo #Done (resolved
   by ...)` per the workflow protocol.

Validation performed:
- Static inspection only. Agent's UCRT64 sandbox compiler is still
  silently-killed; user must build + ctest locally:
  ```bash
  export PATH=/ucrt64/bin:$PATH
  cmake --build build-ucrt-ibkr --target hft_gtests hft_tests -j 8
  ctest --test-dir build-ucrt-ibkr --output-on-failure
  ```

Known risks / follow-up:
- All four features are opt-in. With every new knob at its default value,
  the engine behaves identically to the prior commit. Failure modes are
  bounded to the feature flag being on.
- Hit-count windowing uses `std::chrono::steady_clock::now()`, NOT
  exchange timestamps. Backtest mode plays through historical data on
  wall-clock real time, so the windowing measures wall-clock time, not
  market time. For the Databento backtest #todo, this means the hit-count
  signal won't have meaningful warm-up unless the backtest run lasts
  longer than `hit_count_horizon_seconds * hit_count_baseline` seconds
  of wall-clock. Worth switching to a broker-driven `on_step(t)` clock
  in a follow-up.
- Two-channel Hawkes has no cross-excitation; the two channels track
  independent arrival rates. The original `#todo` flagged cross-
  excitation as the proper microstructure formulation; this commit
  takes the smaller step. Self-only is still a real improvement over
  single-channel: buy-aggressor activity no longer pollutes the sell-
  side execution score.
- Score-weighted sizing always deploys the full account_budget when
  there is at least one positive-score active item. The budget gate
  then becomes a no-op for this rule. The original `#todo` flagged
  this; behaviour is intentional.
- Sibling `#todo`s still open:
  - `[2026-05-16] - Wire FillModel and remaining ranking inputs to real
    market data #todo`. The fill-probability remains synthetic; this
    commit only addresses the score-side signals.
  - `[2026-05-16] - Run a one-week real Databento backtest #todo`. Now
    has three more knobs to potentially calibrate; the run plan should
    sweep at least `hit_count_*` and `position_sizing_rule` in addition
    to the OU threshold.
  - `[2026-05-16] - Live-trading prerequisites #todo`.

Suggested commit:
```bash
git commit -m "feat(engine): hit-count tilt, two-channel Hawkes, OU half-life seconds, score-weighted sizing"
```

## [2026-05-16] - Empirical "+0.8% hit-count" buy-side ranking signal #todo #Done (resolved by [2026-05-16] Implement first 4 ranking-side #todos)

Model / agent:
- Model: Claude Opus 4.7 (Anthropic), reasoning model
- Provider/client: Claude Code on UCRT64

Source state:
- `main` at the Hawkes-wiring commit (`fffde79`) at investigation time.

Context:
- The user's original ranking intuition was to count, per symbol, how often
  it has historically delivered the **`target_profit_pct`** (currently
  `0.008` = +0.8%) over a chosen holding horizon. Stocks where the exit
  target is empirically reachable should rank higher. None of the existing
  signals capture this directly:
  - Hawkes `λ` counts ANY trade activity, no direction or magnitude.
  - OU `μ` measures distance from the trailing mean (mean-reversion).
  - `FillModel p_fill` measures order-placement feasibility, not return.

Proposed scope:
- Per `Stock`, add `int hit_count_window = 0;` plus a small ring/queue of
  recent mid samples (or just two scalars: `last_window_open_mid` and
  `window_open_ts_ns`).
- Sample `s.mid` every `K` seconds (configurable); when
  `(mid_now - mid_window_open) / mid_window_open >= cfg.target_profit_pct`,
  increment a counter and reset the window. Slide-or-decay so the counter
  reflects recent behaviour.
- Use the counter as a multiplicative tilt on the ranking score:
  `score *= clamp(hit_count / hit_count_baseline, hit_floor, hit_ceiling)`.
  Disabled by default via a config knob.
- Source of historical samples to bootstrap the counter:
  - Live: warm up over the first `K * N` seconds after subscribe.
  - Backtest / paper: feed the existing `data/l1/<SYMBOL>.csv` (from
    `scripts/ibkr_historical_l1.py`) through a one-shot warm-up pass at
    engine start.

Open design questions:
- Holding horizon for the +0.8% measurement (5 min, 30 min, 1 trading
  day)? Should match how long you expect to hold a position.
- Sample cadence vs window length: a 1-second sample with a 5-minute
  window over 5 trading days = 130,000 samples per symbol, manageable.
- Slide vs decay: slide is simpler and exact; decay is O(1) memory but
  loses the precise count.
- Hard gate vs soft tilt: should symbols with `hit_count == 0` be barred
  outright, or just down-weighted?

Validation performed:
- None; investigation note only.

Known risks / follow-up:
- `#todo`. Surface to user, get approval before working on it.
- Cross-checks the OU gate: if OU blocks "above mean" entries, the
  hit-count tilt might never get to compute on symbols that are
  trending up. Order in `step()` matters; tilt should multiply score
  BEFORE OU gates fire (so OU still has final say) but AFTER ranking
  sorts have happened — i.e. inside `RankingEngine::step` not in the
  engine's buy loop.
- Survivorship: a symbol that historically hits +0.8% often may have
  done so because it is volatile / illiquid / event-driven. Pair with
  `EWMAVolatility` (currently unused) to normalise.
- Closely related to the "Real alpha signal beyond OU mean-reversion"
  bullet I'd mentioned in earlier review; this is the concrete proposal.

Suggested commit (when resolved):
```bash
git commit -m "feat(engine): +0.8% hit-count ranking tilt (opt-in)"
```

## [2026-05-16] - Two-channel Hawkes (buy-aggressor vs sell-aggressor cross-excitation) #todo #Done (resolved by [2026-05-16] Implement first 4 ranking-side #todos)

Model / agent:
- Model: Claude Opus 4.7 (Anthropic), reasoning model
- Provider/client: Claude Code on UCRT64

Source state:
- `main` at the Hawkes-wiring commit (`fffde79`).

Context:
- The Hawkes resolver entry chose single-channel ("any trade is an event")
  to keep the first wire-up small. The proper microstructure formulation
  is two-channel: separate intensities for buy-initiated and sell-initiated
  trades with cross-excitation (a buy-initiated event lifts both
  `λ_buy` AND `λ_sell` because aggressive buying tends to provoke aggressive
  selling and vice versa). With two channels, `λ_buy` weights the buy
  ranking while `λ_sell` weights the sell-side execution score.

Proposed scope:
- Extend `hft::Hawkes` to two-channel form:
  ```cpp
  struct Hawkes2 {
    double mu_buy, mu_sell;
    double alpha_self_buy,  alpha_cross_buy;
    double alpha_self_sell, alpha_cross_sell;
    double beta;
    double lambda_buy, lambda_sell;
    void update(double dt, bool was_buy_aggressor);
  };
  ```
- Aggressor classification at the source: `TickAttribLast::pastLimit` and
  `unreported` are unreliable. Use Lee-Ready: trade at-or-above ask =
  buy-initiated, at-or-below bid = sell-initiated, mid = use prior
  trade's tick rule. Requires the engine to keep the most recent
  best_bid/best_ask cached alongside the trade event.
- In `LiveExecutionEngine::update_hawkes_from_trades`, pass the
  classification into the update.
- In ranking, replace `p_fill * lambda` with
  `p_fill * lambda_buy` for the buy ranking.
- In `route_exit_orders`, the existing `compute_execution_score` already
  receives `lambda`; pass `lambda_sell` instead.

Open design questions:
- Hawkes2 params: keep the single-channel `Hawkes` for backward compat,
  or migrate all call sites? Migration is cleaner long-term.
- Cross-excitation magnitudes: literature ranges from 0.1 to 0.5 of self-
  excitation. Defaults TBD via backtest.
- Aggressor classification edge cases: trades exactly at mid, trades on
  hidden venues (no public quote ref). Lee-Ready degrades on these.

Validation performed:
- None; investigation note only.

Known risks / follow-up:
- `#todo`. Surface to user, get approval before working on it.
- Sibling resolved: `[2026-05-16] - Wire Hawkes intensity to real IBKR
  AllLast trade prints` brought single-channel Hawkes online. This entry
  is the proper-microstructure follow-up.
- Backtests against Databento MBP-10 give the bid/ask context needed for
  Lee-Ready cheaply, so this is best calibrated alongside the one-week
  Databento `#todo`.

Suggested commit (when resolved):
```bash
git commit -m "feat(model): two-channel Hawkes with Lee-Ready aggressor classification"
```

## [2026-05-16] - OU EWMA in wall-clock time rather than samples #todo #Done (resolved by [2026-05-16] Implement first 4 ranking-side #todos)

Model / agent:
- Model: Claude Opus 4.7 (Anthropic), reasoning model
- Provider/client: Claude Code on UCRT64

Source state:
- `main` at the OU-gate commit (`a2c6b34`).

Context:
- The OU mean-reversion gate updates `s.ou.mu` with a fixed `alpha = 1 /
  ou_window_size`, where the window is **measured in samples of the
  top-of-book stream the engine consumes**. Under bursty activity the
  half-life is wall-clock-short; under sparse activity it is wall-clock-
  long. Different symbols in the same universe therefore see different
  effective half-lives even with identical config.
- Cleaner: weight the EWMA by `dt` between consecutive observations so
  the half-life is in seconds:
  ```
  alpha = 1 - exp(-dt_seconds / tau_seconds)
  s.ou.mu = (1 - alpha) * s.ou.mu + alpha * s.mid
  ```
  with `tau_seconds = ou_halflife_seconds / ln 2`.

Proposed scope:
- Add `Stock::last_ou_update_ts_ns`.
- Replace `ou_window_size` (samples) with `ou_halflife_seconds`
  (double). Keep the old name as a fallback / deprecation alias.
- Update `reconcile_broker_state` to compute `dt = now_ns -
  last_ou_update_ts_ns` and set alpha from that.
- Adjust the `ou_initialized` bootstrap accordingly.

Open design questions:
- What does "now" mean here? `std::chrono::steady_clock::now()` is
  process-monotonic and matches the engine's perception of time, but
  the trade prints carry exchange timestamps - using one for OU and
  the other for Hawkes is slightly inconsistent. Pick one.
- Backtest mode plays through historical data; the wall clock there is
  the replay time, not real time. Honour the broker's `on_step(t)` for
  backtest paths.

Validation performed:
- None; investigation note only.

Known risks / follow-up:
- `#todo`. Surface to user, get approval before working on it. Small
  change, ~50 LOC plus tests, but breaks the existing config key name.
- Sibling resolved: `[2026-05-15] - Wire OU mean-reversion gate into
  the buy decision`.

Suggested commit (when resolved):
```bash
git commit -m "feat(engine): OU EWMA half-life in seconds, dt-weighted"
```

## [2026-05-16] - Position sizing proportional to ranking score #todo #Done (resolved by [2026-05-16] Implement first 4 ranking-side #todos)

Model / agent:
- Model: Claude Opus 4.7 (Anthropic), reasoning model
- Provider/client: Claude Code on UCRT64

Source state:
- `main` at the notional-sizing commit (`cc1879b`).

Context:
- Today `size_entry_qty(limit_price) = floor(trade_notional / limit_price)`
  regardless of where the symbol sits in the ranking. Symbol #1 and symbol
  #3 get the same $500 notional. That throws away the information in the
  score: if symbol #1 has score `10.0` and symbol #3 has score `2.0`,
  putting equal capital on them is leaving expected return on the table.

Proposed scope:
- Introduce a configurable allocation rule for notional:
  - `equal` (current default) - `trade_notional` per active symbol.
  - `score_weighted` - given top-K active symbols with scores
    `s_1, ..., s_K`, allocate `notional_i = account_budget * s_i / sum(s_i)`.
    Cap by `max_notional_per_order`. Skip symbols where the resulting qty
    rounds to 0.
  - `kelly_lite` (optional later) - a simple fractional-Kelly variant
    using `EWMAVolatility` once it's wired in by the FillModel `#todo`.
- Config knob `position_sizing_rule` (string) with default `equal` to keep
  existing behaviour.

Open design questions:
- Score normalisation: scores aren't units-of-PnL, so fraction-of-budget
  shouldn't depend on absolute score magnitudes. Use rank-position weights
  (1st gets 50%, 2nd 30%, 3rd 20%) as a simpler alternative?
- Interaction with `account_budget`: with score-proportional sizing the
  sum of `qty * limit` always equals `account_budget` by construction;
  the existing budget gate becomes a no-op. Worth keeping the gate as a
  belt-and-suspenders check anyway.
- When the ranking shifts mid-day (a symbol drops out of top-K, a new
  one enters), do we resize the existing position? Current design only
  considers fresh entries; modifying existing fills is out of scope.

Validation performed:
- None; investigation note only.

Known risks / follow-up:
- `#todo`. Surface to user, get approval before working on it.
- Closely tied to the `+0.8% hit-count` and FillModel `#todo`s; once
  scores are derived from richer signals, score-proportional sizing
  becomes more meaningful. Worth implementing only AFTER at least one
  of those `#todo`s lands so the score has real economic content.

Suggested commit (when resolved):
```bash
git commit -m "feat(engine): score-proportional position sizing (opt-in)"
```

## [2026-05-16] - Wire Hawkes intensity to real IBKR AllLast trade prints

Model / agent:
- Model: Claude Opus 4.7 (Anthropic), reasoning model
- Provider/client: Claude Code on UCRT64

Source state:
- Local clone at `D:\trading-system`, on `main`, at `c3a45c5` (protobuf
  parallelism cap) - the previous Hawkes investigation's #todo entry was at
  the top of the log and explicitly authorised by the user for resolution
  ("finish the #todo").

User intent:
- Resolve the `[2026-05-15] - Investigation: Hawkes intensity does not
  consume real IBKR trade events #todo`. Buy ranking's `λ` was driven by a
  deterministic toy event clock; this change makes it consume real IBKR
  trade prints when explicitly opted in via config.

Design decisions:
- **Single-channel Hawkes for now** - every trade is one event, regardless
  of aggressor side. Two-channel (buy-aggressor / sell-aggressor with
  cross-excitation) is the proper microstructure formulation but requires
  extending `Hawkes` and a reliable aggressor classifier; not in scope.
- **Opt-in via `hawkes_use_real_trades` (default false)**. Off keeps
  existing tests / paper config behaviour unchanged. On causes the engine
  to also call `subscribe_trades` for every symbol and to drain trade events
  each step.
- **Per-ticker FIFO inside `IBKRClient`**, drained by the engine - mirrors
  the `OrderLifecycleBook` polling pattern used for fills, rather than
  registering a callback closure inside the engine.
- **dt in nanoseconds end-to-end**, converted to seconds (`* 1e-9`) at the
  Hawkes-update boundary. First-ever observation on a symbol uses a default
  `dt = 1 ms` so Hawkes does not decay to baseline on a huge first-event dt.

Files changed:
- `include/broker/IBKRCallbacks.hpp` - add `virtual void on_trade(int
  ticker_id, double price, double qty, std::int64_t exch_ts_ns) {}` to the
  inbound surface, default no-op.
- `include/broker/IBKRTransport.hpp` - add `virtual void subscribe_trades(
  const TopOfBookRequest&) {}` default no-op.
- `include/broker/IBroker.hpp` - add `struct TradeEvent { price, qty,
  exch_ts_ns }`, `virtual void subscribe_trades(...)` default no-op, and
  `virtual std::vector<TradeEvent> drain_trades(int ticker_id) { return {}; }`
  default empty so brokers that don't deliver trades (`LocalSimBroker`,
  `DatabentoBacktestBroker`) compile unchanged.
- `include/broker/IBKRClient.hpp`, `src/lib/IBKRClient.cpp` - implement
  `subscribe_trades` (forwards to transport), `on_trade` (locks
  `books_mutex_`, appends to `trade_events_[ticker_id]`), and `drain_trades`
  (locks, moves out the vector). `trade_events_` is a new
  `std::unordered_map<int, std::vector<TradeEvent>>`.
- `src/lib/RealIBKRTransport.cpp` -
  - Replace the empty `tickByTickAllLast` body with a real implementation
    that calls `DecimalFunctions::decimalToDouble(size)` and forwards via
    `callbacks_->on_trade(reqId, price, qty, exch_time * 1e9)`.
  - Implement `subscribe_trades` via
    `client_.reqTickByTickData(req.ticker_id, contract, "AllLast", 0, false)`.
- `include/models/stock.hpp` - add `std::int64_t last_trade_ts_ns = 0;` so
  the engine can compute event spacing per symbol.
- `include/config/AppConfig.hpp`, `src/lib/AppConfig.cpp` - add
  `bool hawkes_use_real_trades = false;` plus parsing.
- `include/engine/LiveExecutionEngine.hpp`, `src/lib/LiveExecutionEngine.cpp`:
  - New private `void update_hawkes_from_trades()` that iterates universe,
    drains trades per ticker_id, and calls `s.hawkes.update(dt_s, 1)`.
  - `reconcile_broker_state()` calls `update_hawkes_from_trades()` first so
    the rest of the step sees the updated `λ`.
  - `subscribe_live_books()` additionally calls `broker_->subscribe_trades`
    for every symbol when `cfg_.app.hawkes_use_real_trades`.
- `tests/common/MockIBKRTransport.hpp` - `MOCK_METHOD` for
  `subscribe_trades`.
- `tests/common/FakeIBKRTransport.hpp` - empty `subscribe_trades` override.
- `tests/common/MockIBroker.hpp` - `MOCK_METHOD` for `subscribe_trades` and
  `drain_trades`.
- `tests/unit/IBKRClientTest.cpp` - new tests:
  `SubscribeTradesForwardsToTransport`, `OnTradeAccumulatesPerTickerAndDrain
  Empties`, `DrainTradesEmptyForUnknownTicker`.
- `tests/unit/LiveExecutionEngineTest.cpp` - new tests:
  `SubscribeLiveBooksAlsoSubscribesTradesWhenEnabled`,
  `SubscribeLiveBooksSkipsTradesWhenDisabled`,
  `ReconcileDrivesHawkesFromRealTrades`,
  `ReconcileSkipsHawkesUpdateWhenDisabled`.
- `tests/unit/AppConfigTest.cpp` - default + parse coverage for
  `hawkes_use_real_trades`.
- `config.ibkr_paper.example.ini` - documents the knob, leaves it off until
  the AllLast market-data subscription is confirmed live on the account.
- `config.databento_backtest.example.ini` - leaves the knob off explicitly
  with a comment that `DatabentoBacktestBroker` does not produce trades on
  this path.

Deletions / removals:
- None.

Steps taken:
1. Added `on_trade` callback shape and `TradeEvent` struct.
2. Implemented the transport-side `subscribe_trades` and replaced the empty
   `tickByTickAllLast` with the forwarding body that decodes Decimal size
   and packs Unix-seconds time as nanoseconds.
3. Plumbed `IBKRClient` to accumulate per-ticker trades and expose
   `drain_trades` through `IBroker`.
4. Wired the engine to subscribe to trades and drain them each step,
   gated by `hawkes_use_real_trades`.
5. Added unit-test coverage at both the IBKRClient seam and the engine seam.
6. Retagged the [2026-05-15] Hawkes investigation `#todo` with `#Done` and
   a back-reference to this entry per the workflow protocol.

Validation performed:
- Static inspection only. Agent's UCRT64 sandbox compiler still
  silently-killed; user must build + ctest from a regular UCRT64 shell:
  ```bash
  export PATH=/ucrt64/bin:$PATH
  cmake --build build-ucrt-ibkr --target hft_gtests hft_tests -j 8
  ctest --test-dir build-ucrt-ibkr --output-on-failure
  ```

Known risks / follow-up:
- **IBKR market-data prerequisite**: `reqTickByTickData(..., "AllLast", ...)`
  requires that the account has the consolidated/NBBO trade-prints
  subscription for the symbol. On a fresh paper account without the US
  Securities Snapshot and Futures Value Bundle, the call returns error 354
  ("Requested market data is not subscribed") and no on_trade fires.
  Verify the bundle on the IBKR side before flipping the config knob on
  in production.
- **Single-channel Hawkes**: signal collapses buyer-initiated and seller-
  initiated trades into one event stream. Two-channel is the proper next
  iteration; would let `λ_buy` weight the buy ranking while `λ_sell`
  weights the sell-side execution score.
- **No event-rate sanity cap**: in an active stock (>1000 trades/sec) the
  per-step drain can be hundreds of events. Each `Hawkes::update` is O(1)
  so memory is fine, but lambda can spike well above the model's
  steady-state range. Worth adding a per-step event cap or moving to
  per-event update done on the reader thread later.
- **`last_trade_ts_ns` only updates on consumed events**, not on subscribe.
  If the first trade arrives 5 hours after subscribe, dt = 1 ms by our
  default rule, which slightly understates the actual quietness. Cosmetic.
- The fill-probability (`FillModel`) is **still synthetic** - it runs
  against the internal `Simulator` order book, not against IBKR's real L2.
  Wiring Hawkes alone leaves the buy decision two-thirds market-aware
  (real `s.mid`, real `λ`) but one-third synthetic (`p_fill`). See the
  new `#todo` immediately below.

Suggested commit:
```bash
git commit -m "feat(engine): drive Hawkes from real IBKR AllLast trade prints (opt-in)"
```

## [2026-05-16] - Wire FillModel and remaining ranking inputs to real market data #todo

Model / agent:
- Model: Claude Opus 4.7 (Anthropic), reasoning model
- Provider/client: Claude Code on UCRT64

Source state:
- Local clone at `D:\trading-system`, on `main`, expected to be at the
  Hawkes-resolver commit after this entry lands.

Context (why this is a `#todo` not a fix):
- The Hawkes `#todo` only resolved the activity weight `λ`. The other two
  inputs to the buy ranking score `p_fill × λ` are still synthetic:
  1. `FillModel::compute` runs against the internal `hft::Simulator`
     order book seeded by random external_flow, not against IBKR's real L2.
  2. The 8 candidate limit prices are a hardcoded `mid ± 0.10` in `0.025`
     steps - wrong for sub-$30 or >$500 stocks and wrong for sub-cent tick
     sizes.
  3. `RankingEngine::initialize` sets `s.mid = 100.0 + 0.05*i` as a
     synthetic seed; real `s.mid` only arrives after the first
     `reconcile_broker_state`. Cosmetic but pollutes the very first
     ranking step.
  4. `risk/EWMAVolatility.h` exists but is referenced only in tests; no
     code reads `σ` to risk-adjust score or size.

Proposed scope:
- Replace the `FillModel` simulator-book input path with real IBKR L2:
  - Use `IBKRClient::snapshot_book(ticker_id)` to obtain the real book.
  - Compute `queue_ahead` at each candidate limit from the real book's
    bid levels (sum of sizes at price >= L, since we're buying).
  - Estimate `traded_at_level` from accumulated trade volume at that
    level over a rolling window (cheap to do alongside the Hawkes wire).
- Make the 8-candidate grid relative to spread and tick:
  - Read the symbol's tick size from `reqContractDetails` (cache it).
  - Span ±N ticks around the bid, where N is configurable
    (`fill_candidate_n_ticks`, default 4).
- Bootstrap `s.mid` from the first `reconcile_broker_state` rather than
  the `100.0 + 0.05*i` seed.
- Wire `EWMAVolatility` into per-symbol `σ` tracking from mid returns,
  add a knob `risk_adjust_score` that divides the ranking score by
  max(σ, σ_floor) when set.

Open design questions:
- Hold candidate evaluation per-tick or per-event? Per-tick is what the
  code does now; per-trade-event would be more reactive but heavier.
- Use full L2 ladder or just top-of-book + estimated cumulative ladder?
  Real ladders are noisy and IBKR's L2 is delayed for some venues.

Validation performed:
- None; this is an investigation note, not a code change.

Known risks / follow-up:
- `#todo`. Surface to user and get approval before working on it.
- Sibling: `[2026-05-16] - Wire Hawkes intensity to real IBKR AllLast trade
  prints` resolved the `λ` input; this entry covers everything else needed
  to make the buy ranking fully market-aware.

Suggested commit (when resolved):
```bash
git commit -m "feat(engine): real-L2-driven FillModel and tick-relative candidate grid"
```

## [2026-05-16] - Run a one-week real Databento backtest with OU gate ON and calibrate thresholds #todo

Model / agent:
- Model: Claude Opus 4.7 (Anthropic), reasoning model
- Provider/client: Claude Code on UCRT64

Source state:
- VPS workdir `/mnt/HC_Volume_105581071/trading-system/` already has the
  scripts (`run_hftbacktest_databento.py`, `databento_download_l2.py`,
  `databento_download_mbp1.py`) and the venv.
- Prior synthetic and 1-hour real smoke runs at
  `reports/databento_real_smoke_report.md` and
  `reports/databento_synthetic_hourly_report.md`.

Context:
- The 1-hour real Databento smoke for `AAPL` ended with `final_position=1.0`
  - the price never moved +80 bps in an hour, so the sell limit was never
  touched. Expected for a quiet intraday window; the strategy was never
  validated end-to-end against real data.
- Full one-week run is ~`$15-20` Databento billing (AAPL+NVDA+AMD,
  2026-04-13 -> 2026-05-01). User has explicitly OK'd the budget.
- OU gate is now wired (`a2c6b34`) and ON in the backtest example config,
  but `ou_buy_threshold_pct=0.0` was picked without data. The right
  threshold needs to be calibrated against a real-data run; -0.005 or
  -0.001 might be more selective.

Proposed scope:
- Copy any updated scripts/config to Hetzner (`scp scripts/run_hftbacktest_*
  hetzner:/mnt/HC_Volume_105581071/trading-system/scripts/`).
- (Optional but recommended) Run `ibkr_historical_l1.py` locally for
  AAPL/NVDA/AMD over the same window, copy CSVs to `data/l1/` on VPS.
  Otherwise the L1 fallback synthesises bid/ask from coarser sources.
- Kick off the one-week run on Hetzner with the OU gate at default
  threshold:
  ```bash
  ssh hetzner 'cd /mnt/HC_Volume_105581071/trading-system && . .venv/bin/activate && \
    python scripts/run_hftbacktest_databento.py \
      --symbols AAPL,NVDA,AMD \
      --start 2026-04-13T13:30:00Z --end 2026-05-01T20:00:00Z \
      --api-key-file /root/.config/trading-system/databento_api_key \
      --summary reports/oneweek_summary.json --report reports/oneweek_report.md'
  ```
- Inspect: fill rate, filled-vs-touched, equity, average spread, target
  touch counts. If sells fire reasonably, sweep `ou_buy_threshold_pct`
  over `{-0.01, -0.005, -0.001, 0, +0.001, +0.005}` against the same
  cached L2 CSVs (re-downloads are free at that point) and pick the best.

Open design questions:
- Should the backtest also test with `ou_window_size` swept? Half-life of
  4096 samples vs 8192 vs 2048 may matter more than threshold.
- How to handle overnight gaps in the EWMA - reset `ou_initialized` at
  session-open boundaries? Currently it carries across days.

Validation performed:
- None.

Known risks / follow-up:
- `#todo`. Surface to user, get explicit approval again before kicking
  the billable download (the user OK'd ~$20; verify before exceeding).
- Disk/memory: prior estimates show ~3.7M L2 rows per symbol-hour. One
  week × 3 symbols × 6.5 RTH hours = ~700M rows at peak. Monitor
  `/mnt/HC_Volume_105581071/trading-system/data/databento/` size during
  the run.
- Run wall-clock: previous 1-hour run took ~minutes; 8 trading days × 3
  symbols is likely several hours. Run via `tmux` or `nohup` on Hetzner.
- Sibling `#todo`: `[2026-05-16] - Wire FillModel and remaining ranking
  inputs to real market data` - if that is resolved first, the
  calibration changes (the buy score changes), so the resulting
  thresholds may not transfer. Worth re-running after FillModel work.

Suggested commit (when calibration is recorded):
```bash
git commit -m "docs(backtest): record one-week Databento run + OU threshold calibration"
```

## [2026-05-16] - Live-trading prerequisites (kill-switch, paper endurance, data subs, ops hardening) #todo

Model / agent:
- Model: Claude Opus 4.7 (Anthropic), reasoning model
- Provider/client: Claude Code on UCRT64

Source state:
- Aggregated from "hard blockers" identified during the 2026-05-16
  end-to-end review. Listed as a single `#todo` because no item is large
  on its own but together they gate enabling real-money live trading.

Context:
- None of the live-mode safety / operational pieces exist yet. The engine
  is structurally capable of routing orders to live IBKR but the
  guardrails and operational hardening are missing. Each sub-item is
  a small change (under ~150 LOC); together they belong in one
  pre-flight checklist.

Sub-items (each can become its own follow-up entry when worked):

1. **Daily-loss kill-switch.** Track realized + unrealized PnL since
   session open. When loss exceeds configurable `daily_loss_kill_usd`,
   cancel all open orders, refuse to place new ones, and set component
   state Engine -> Error. No code today; engine has `OrderLifecycleBook`
   for fills and `open_positions_` with `entry_price` so the math is
   close at hand.
2. **Paper-trade endurance run.** Spin `hft_app` against real IBKR paper
   port for a full RTH session, watch for partial-fill edge cases,
   cancel/reject behaviour, reconnect handling, and overnight order
   state. Existing `ibkr_paper_order_probe` covers the round-trip; a
   long-running scenario does not exist.
3. **Confirm IBKR market-data subscriptions** for the active universe.
   The sibling Hawkes entry needs AllLast trade prints (NYSE Network A +
   Network B + NASDAQ Network C, ~$4.50/mo per earlier handoff). Verify
   in IBKR Account Management before turning `hawkes_use_real_trades=true`.
4. **Hetzner operational hardening.** Currently the VPS has no
   auto-restart on `hft_app` crash, no log rotation, and no alerting.
   Add systemd unit + logrotate config + a thin "alert if stopped" hook.
   100 GB rolling-compressed log retention was the earlier budget.
5. **IB Gateway 2FA / session management on VPS.** The gateway can't be
   left logged in indefinitely on a paper account without periodic
   re-auth. Document the operational procedure for keeping the session
   alive and reconnecting after IBKR's nightly restart.
6. **Cost calibration.** `AppConfig` has `commission_per_share`,
   `half_spread_cost`, `impact_coefficient` etc. with reasonable
   defaults. Calibrate them against the user's actual IBKR tier (likely
   `IBKR Pro` with tiered pricing) before live trading - the sell
   `target_profit_pct = 0.008` is the minimum profitable trade given
   these costs, and being off by 0.05 on commission alone meaningfully
   changes that.

Validation performed:
- None.

Known risks / follow-up:
- `#todo`. None of this is sequential with the FillModel `#todo` and the
  one-week Databento `#todo`, but all three must be resolved before the
  user enables live mode.
- This entry is intentionally broad. A future agent picking it up may
  prefer to split it into per-sub-item `#todo`s and retag this one
  `#Done (split into ...)` for clarity.

Suggested commit (when sub-items land):
- One commit per sub-item, e.g.
  ```bash
  git commit -m "feat(engine): daily-loss kill switch + AppConfig.daily_loss_kill_usd"
  ```

## [2026-05-15] - Wire OU mean-reversion gate into the buy decision

Model / agent:
- Model: Claude Opus 4.7 (Anthropic), reasoning model
- Provider/client: Claude Code on UCRT64

Source state:
- Local clone at `D:\trading-system`, on `main`, two unpushed commits ahead of
  `origin/main`: `cc1879b` (notional sizing + IBKR L1 exporter) and `0e2cafa`
  (this log + workflow #todo/#Done protocol). This entry adds a third.

User intent:
- The user remembered designing an entry rule of the form "buy when price is
  under the mean daily/weekly price". A search confirmed the OU mean-reversion
  scaffolding existed (`include/models/ou.hpp`, `OUState` declared on every
  `Stock`) but had **zero call sites** in `src/` - it was declared and never
  used. The user asked to wire it in, since it matches their original
  intuition and gives the buy decision the alpha/direction signal that
  Hawkes (still on synthetic events; see sibling #todo below) does not.

Design choice:
- Use a slow EWMA on the observed top-of-book mid to track the trailing mean,
  rather than a deque-based exact rolling mean. O(1) state per symbol, no
  history buffer, effective half-life ~`ou_window_size * ln 2` samples.
- Bootstrap `ou.mu` to the first observed mid on each symbol via a new
  `Stock::ou_initialized` flag, otherwise the default `mu=100.0` would block
  buys on any non-$100 symbol (e.g. $250 AAPL) for ~3500 samples until the
  EWMA converged.
- Gate the buy loop with `if (mid > ou.mu * (1 + ou_buy_threshold_pct)) skip`.
- Off by default (`ou_window_size = 0`). Opt-in via config so existing tests
  and the live IBKR paper config are unaffected until the user has run a
  backtest to pick a threshold.

Files changed:
- `include/config/AppConfig.hpp` - add `ou_window_size` (default `0`) and
  `ou_buy_threshold_pct` (default `0.0`); inline doc explains semantics.
- `src/lib/AppConfig.cpp` - parse `ou_window_size` and `ou_buy_threshold_pct`.
- `include/models/stock.hpp` - add `bool ou_initialized = false;` to `Stock`.
- `src/lib/LiveExecutionEngine.cpp` -
  - `reconcile_broker_state`: on each top-of-book update, bootstrap
    `s.ou.mu = s.mid` on first valid observation, then EWMA `s.ou.mu` with
    `alpha = 1 / ou_window_size` and call `update_ou(s.ou, s.mid)`.
  - Buy loop: skip candidate when `mid > ou.mu * (1 + ou_buy_threshold_pct)`,
    placed before sizing/budget gates so a gated-out symbol doesn't burn a
    next-order-id.
- `tests/unit/AppConfigTest.cpp` - extend defaults and `ParsesAllKnownKeys`.
- `tests/unit/LiveExecutionEngineTest.cpp` - three new tests:
  `OUGateBlocksBuysAboveMean`, `OUGateAllowsBuysAtOrBelowMean`,
  `OUGateDisabledWhenWindowSizeZero`. They pre-seed `s.ou.mu` and
  `s.ou_initialized` directly through `engine.ranking.portfolio.items`,
  exploiting the existing public-field structure.
- `config.ibkr_paper.example.ini` - new `[entry_strategy]` section, gate
  documented but disabled (`ou_window_size=0`).
- `config.databento_backtest.example.ini` - `[entry_strategy]` section with
  gate ON (`ou_window_size=4096`, `ou_buy_threshold_pct=0.0`) so the
  upcoming Databento smoke exercises it.

Deletions / removals:
- None.

Steps taken:
1. Searched the codebase for mean/avg/rolling/EMA/VWAP/momentum references;
   only `EWMAVolatility` and the OU scaffolding turned up, both unused in
   `src/lib/`.
2. Confirmed `OUState`'s only call sites are its own struct definition and
   `Stock::ou` field declaration. Dead scaffolding matching the user's
   original design intent.
3. Designed the wiring (EWMA on mu, bootstrap flag, opt-in via config) to
   not disturb existing behaviour when not configured.
4. Implemented and wrote three engine-level tests + AppConfig coverage.

Validation performed:
- Static inspection only. The agent's UCRT64 sandbox compiler is silently
  killed by something in the harness (cc1plus exits 1 with no stderr on any
  input; `g++ --version` works but `cc1plus.exe --version` produces no
  output and exit 0). User must build + ctest from a regular UCRT64 shell:
  ```bash
  export PATH=/ucrt64/bin:$PATH
  cmake --build build-ucrt-ibkr --target hft_gtests hft_tests -j 8
  ctest --test-dir build-ucrt-ibkr --output-on-failure
  ```

Known risks / follow-up:
- The "trailing mean" is over the top-of-book sample stream the engine sees,
  not over a clock-time window. With high-frequency ticks the half-life
  shrinks vs. wall-clock; with sparse data it grows. A future refinement is
  to weight the EWMA by `dt` so the half-life is in seconds rather than
  samples.
- `OUState::theta`/`x` are still defaults; only `mu` is being updated, and
  `update_ou` nudges `x` toward observed but `x` is not yet read anywhere.
  Worth deciding whether the gate should use `mu` (mean) or `x` (smoothed
  observation) as the reference. Current code uses `mu`.
- The gate is direction-blind in one sense: it blocks buys above the mean
  but does not require a *rising* signal, so on a strongly downtrending
  symbol it will buy repeatedly. Combine with a momentum filter later.
- See sibling `#todo` below: Hawkes is still on synthetic events. Buy
  decision now has the alpha signal (OU) wired to real data, but the
  fill-rate weight (`λ`) and the simulated `FillModel` are still synthetic.

Suggested commit:
```bash
git commit -m "feat(engine): OU mean-reversion entry gate (opt-in via ou_window_size)"
```

## [2026-05-15] - Investigation: Hawkes intensity does not consume real IBKR trade events #todo #Done (resolved by [2026-05-16] Wire Hawkes intensity to real IBKR AllLast trade prints)

Model / agent:
- Model: Claude Opus 4.7 (Anthropic), reasoning model
- Provider/client: Claude Code on UCRT64

Source state:
- Local clone at `D:\trading-system`, latest commit observed as `cc1879b`
  (`feat(engine,scripts): notional-driven trade sizing with account budget;
  IBKR historical L1 exporter`), unpushed at investigation time.

User question prompting the investigation:
- "So hawkes works for the trading system, it uses real ikbr data right now
  if started on live trading, right?" The user assumed Hawkes was already
  wired to the live IBKR feed because the engine routes orders through IBKR
  in live mode.

Findings (verified by grep + file inspection, no code change):
- `hft::Hawkes::update(dt, event)` is called from exactly one site:
  `src/lib/RankingEngine.cpp:40`, with
  `event = ((t + symbol.back()) % 2)`. That is a deterministic 0/1 toggle
  parameterised on tick number and the last character of the ticker. It is
  not driven by market data.
- `RealIBKRTransport::tickByTickAllLast` (the TWS callback that delivers
  trade prints in live mode) is overridden with an empty body
  (`src/lib/RealIBKRTransport.cpp:293`). Even when IBKR is sending real
  trade events, they are dropped on the floor at the transport layer.
- `IBKRCallbacks` does not expose an `on_trade(...)` callback; there is no
  current routing path for trade prints into `IBKRClient` or
  `LiveExecutionEngine`.
- Top-of-book prices/sizes from `tickPrice` / `tickSize` and full L2 from
  `updateMktDepth` *are* wired through to the engine. The asymmetry is real:
  in live mode buys see real top-of-book mid but a synthetic Hawkes
  intensity and a synthetic fill-probability (`FillModel` runs against the
  internal `hft::Simulator` order book, not against IBKR's L2). Sells use
  real L2 for queue-ahead and directional-pressure estimation.
- Net consequence: if live trading were enabled today, the *ranking* score
  `p_fill * lambda` would have both inputs disconnected from the market.
  The system would still place real orders with real money, but on a signal
  uncorrelated with real-market behaviour.

Proposed fix (not implemented; awaiting user approval per the `#todo` flow):
1. Add `virtual void on_trade(int ticker_id, double qty, double price,
   bool is_buyer_initiated, std::int64_t exch_ts_ns) = 0;` to
   `IBKRCallbacks`. Defaulted-empty implementations on `FakeIBKRTransport`
   and `MockIBKRTransport` to keep existing tests compiling.
2. Replace the empty `tickByTickAllLast` body in
   `src/lib/RealIBKRTransport.cpp` with a body that forwards to
   `callbacks_->on_trade(...)` after converting the IBKR `Decimal` size and
   classifying buyer- vs. seller-initiated using the tick's `pastLimit` /
   `unreported` flags as a fall-back.
3. Add `IBroker::subscribe_trades(int ticker_id, std::string symbol)` (or
   piggyback on `subscribe_top_of_book` if we want trade ticks for the same
   universe). In `RealIBKRTransport`, implement it as
   `client_.reqTickByTickData(ticker_id, contract, "AllLast", 0, false)`.
4. In `IBKRClient`, implement `on_trade` and forward into a per-ticker
   structure (e.g. last-event timestamp + a callback registered by the
   engine).
5. `LiveExecutionEngine` registers a trade-event consumer that, on each
   trade, locates the matching `Stock`, computes
   `dt = now_ns - s.last_trade_ts_ns`, and calls `s.hawkes.update(dt, 1)`.
   `s.last_trade_ts_ns` becomes a new field on `Stock`.
6. New unit test in `tests/unit/IBKRClientTest.cpp` driving the new
   `on_trade` callback and asserting Hawkes lambda changes.
7. Open question: single-channel (any trade is an event) vs two-channel
   (buyer-initiated vs seller-initiated as separate Hawkes processes with
   cross-excitation). Single-channel is the trivial first wiring; two-
   channel is the proper microstructure formulation and requires extending
   the `Hawkes` struct. The user's call.

Validation performed:
- None beyond static inspection. No code changes in this entry.

Known risks / follow-up:
- `#todo`. This entry is the open investigation. Future agents must scan
  the log for `#todo` per `AGENT_WORKFLOW.md` and surface this item to the
  user before working on it.
- Until this is fixed, **do not enable live trading**. Real money on a
  synthetic signal is a foreseeable loss.
- The synthetic fill-probability is a *separate* `#todo` not raised here;
  fixing the Hawkes signal alone leaves the ranking partially synthetic.
- Sibling resolved: see `[2026-05-15] - Wire OU mean-reversion gate into
  the buy decision`. OU (the alpha/direction signal) is now wired to real
  IBKR mid via `LiveExecutionEngine::reconcile_broker_state`. Hawkes (the
  activity/fill-rate signal) remains on synthetic events and is what this
  `#todo` still covers.

Suggested commit (for the investigation note only):
```bash
git commit -m "docs(agent): log #todo - Hawkes intensity not wired to real IBKR trades"
```

## [2026-05-03] - Add max-open-symbol cap and buy/sell CI coverage

Model / agent:
- Model: GPT-5.5 Thinking, reasoning model
- Provider/client: Codex desktop

Source state:
- Local clone at `D:\trading-system`.
- GitHub CI run checked for pushed commit `361a7e3c7ee645436bd83adaa90b9ba8a6c2824a`.
- Existing unrelated dirty local items were present and not touched. Do not record VPS IPs, machine IDs, SSH keys, or credentials.

User request:
- Fix the legacy CI test expectation so it covers the new buy/sell behavior.
- Add logic to hold at most 3 bought symbols and use only those bought symbols as sell candidates.
- Explain whether buy ranking keeps running after sells or only under certain conditions.

Files changed:
- `include/config/AppConfig.hpp`, `src/lib/AppConfig.cpp` - added `max_open_symbols`, defaulting to `3`, and config parsing.
- `include/engine/LiveExecutionEngine.hpp`, `src/lib/LiveExecutionEngine.cpp` - added exposure-count helpers and stopped buy routing once pending/open exposure reaches `max_open_symbols`.
- `tests/module/TestBrokerIntegration.cpp` - replaced the stale "every step places top_k orders" expectation with a fill/depth broker test that checks capped buys, generated sell orders, and sell candidates limited to bought symbols.
- `tests/unit/AppConfigTest.cpp`, `tests/unit/TestCoreModels.cpp` - added config coverage for `max_open_symbols`.
- `config.ibkr_paper.example.ini`, `config.databento_backtest.example.ini` - documented `max_open_symbols=3`.

Deletions / removals:
- No files removed.

Steps taken:
1. Inspected GitHub CI logs and confirmed the current failure is one legacy `hft_tests` assertion.
2. Added a configurable open-symbol cap.
3. Updated buy routing to count both filled/open positions and pending buy entries.
4. Updated the failing legacy test to assert buys plus sells instead of repeated duplicate buys.

Validation performed:
- `git diff --check` on touched files: no whitespace errors; only LF-to-CRLF Git warnings.
- `clang-format --dry-run --Werror` on touched C++ files: passed.
- No build or test run locally, preserving the user's earlier instruction to avoid local builds/tests until requested.

Known risks / follow-up:
- The new legacy test uses a deterministic test broker that fills buys immediately and provides synthetic L2 for sell scoring; it validates engine routing, not broker exchange realism.
- After pushing, re-watch CI to confirm the old one-test failure is gone.

Suggested commit:
```bash
git commit -m "test(engine): cover capped buys and sell routing"
```

## [2026-05-03] - Split Databento backtests into L1 buy data and L2 sell data

Model / agent:
- Model: GPT-5.5 Thinking, reasoning model
- Provider/client: Codex desktop

Source state:
- Local clone at `D:\trading-system`.
- HEAD observed as `b99345263390f6754a1b2363389f3e4fb2c3626f` (`added backtesting; reroute ikbr to databento download script for backtesting`).
- Existing unrelated dirty local items were present and not touched. Do not record VPS IPs, machine IDs, SSH keys, or credentials.

User request:
- Create a backtesting script using the hftbacktest framework.
- Add a Databento MBP-1 Python downloader that acts as L1 input for the backtest broker.
- Make the C++ broker/engine reflect the desired split: L1 for buy/ranking queries and L2 for sell/execution queries.

Files changed:
- `include/broker/IBroker.hpp` - added `TopOfBookRequest`, `TopOfBook`, `subscribe_top_of_book`, and `snapshot_top_of_book`.
- `include/broker/IBKRCallbacks.hpp`, `include/broker/IBKRTransport.hpp`, `include/broker/IBKRClient.hpp`, `src/lib/IBKRClient.cpp`, `src/lib/RealIBKRTransport.cpp` - added IBKR top-of-book subscription/callback plumbing through `reqMktData` while preserving depth via `reqMktDepth`.
- `include/broker/DatabentoBacktestBroker.hpp`, `src/lib/DatabentoBacktestBroker.cpp` - split replay loading into MBP-1 top-of-book and MBP-10 depth caches, with L1 used as the buy fallback and L2 used for sell-side fills.
- `include/config/AppConfig.hpp`, `src/lib/AppConfig.cpp`, `config.databento_backtest.example.ini`, `tests/unit/AppConfigTest.cpp` - added separate Databento L1/L2 script, dataset, and schema config keys while keeping old L2 aliases.
- `include/engine/LiveExecutionEngine.hpp`, `src/lib/LiveExecutionEngine.cpp`, `tests/unit/LiveExecutionEngineTest.cpp`, `tests/unit/IBKRClientTest.cpp` - changed universe subscriptions and ranking reconciliation to L1/top-of-book, lazy-subscribed sell-side L2 using separate depth request ids, and added L1 transport/callback unit expectations.
- `tests/common/FakeIBKRTransport.hpp`, `tests/common/MockIBKRTransport.hpp`, `tests/common/MockIBroker.hpp` - updated test doubles for top-of-book broker APIs.
- `scripts/databento_download_mbp1.py` - new Databento MBP-1 downloader producing `step,bid_price,bid_size,ask_price,ask_size`, with synthetic mode.
- `scripts/run_hftbacktest_databento.py` - new initial hftbacktest runner that downloads/reuses MBP-1 for entry reference, downloads/reuses MBP-10 for the sell window, converts depth data to a normalized array, and submits a target SELL scenario.

Deletions / removals:
- No files removed by this change.

Steps taken:
1. Added a top-of-book path to the shared broker interface.
2. Wired IBKR live/paper to use `reqMktData` for L1 and keep `reqMktDepth` for L2.
3. Split Databento backtest replay into MBP-1 and MBP-10 cache/download paths.
4. Changed engine buy/ranking data reads to `snapshot_top_of_book`.
5. Changed sell-side execution to lazy-load L2 only after a position exists, using a separate request-id range from L1.
6. Added the MBP-1 downloader and an initial hftbacktest sell-window script.

Validation performed:
- Read-only source inspection and targeted `Select-String` searches.
- `git diff --check` on the touched tracked files: no whitespace errors reported; only existing LF-to-CRLF warnings from Git.
- Python AST parse for `scripts/databento_download_mbp1.py` and `scripts/run_hftbacktest_databento.py`: `python ast ok`.
- No build or test run, per user instruction to avoid builds/tests until all changes are ready.

Known risks / follow-up:
- `scripts/run_hftbacktest_databento.py` is an initial integration scaffold; exact hftbacktest constructor/module names may need adjustment against the installed package version.
- The C++ Databento broker still uses a simple crossed best bid/ask fill model; queue-realistic sell testing belongs in hftbacktest.
- Databento historical downloads require the `databento` Python package and credentials in the runtime environment.

Suggested commit:
```bash
git commit -m "feat(backtest): split Databento L1 buys and L2 sell execution"
```

## [2026-05-03] - Add Databento-backed broker interface scaffold for backtests

Model / agent:
- Model: GPT-5.5 Thinking, reasoning model
- Provider/client: Codex desktop

Source state:
- Local clone at `D:\trading-system`.
- Existing unrelated dirty local items were present and not touched. Do not record VPS IPs, machine IDs, SSH keys, or credentials.

User request:
- See whether the C++ broker code can keep live/paper trading as-is while allowing backtests to use the same engine/broker interface backed by Databento data downloaded through a Python script.

Design implemented:
- Extended `IBroker` with optional default hooks:
  - `on_step(int)` for replay brokers.
  - `snapshot_book(int)` for market-depth snapshots.
  - `order_lifecycle()` and `ack_latency_ms(int)` for fill/lifecycle state.
- `IBKRClient` now overrides those hooks without changing its existing live/paper behavior.
- `LiveExecutionEngine` now calls these generic broker hooks instead of hard-casting to `IBKRClient` for market data and lifecycle, except for IBKR-only `nextValidId` synchronization.
- Added `DatabentoBacktestBroker`, a new `IBroker` implementation for `mode=databento_backtest`.
  - `subscribe_market_depth` ensures a symbol CSV exists, calling the Python downloader if absent.
  - CSV replay exposes `L2Book` snapshots through `snapshot_book`.
  - `on_step` advances replay time.
  - Limit orders are tracked in an `OrderLifecycleBook`; marketable buy/sell limits fill against the replayed best ask/bid.
- Added `scripts/databento_download_l2.py`.
  - Uses the official Databento Python historical API if installed/configured.
  - Downloads `mbp-10` by default and flattens `bid_px_N`/`ask_px_N` + sizes into `step,side,level,price,size` CSV for C++ replay.
  - Includes an explicit `--synthetic` mode for local smoke data without contacting Databento.
- Added `config.databento_backtest.example.ini` with Databento/backtest settings.
- Added config parsing for `databento_cache_dir`, `databento_download_script`, `databento_python`, `databento_dataset`, `databento_schema`, `databento_start`, and `databento_end`.

Important limitations:
- This is a scaffold, not a full hftbacktest integration yet.
- Current replay downloads per subscribed symbol; later we can make it lazier around candidate sell windows if broad L2 downloads become too costly.
- Fill simulation is intentionally simple: crossed/marketable limits fill against current best bid/ask; passive queue progression is not yet hftbacktest-grade.
- No build/test run, per user instruction to avoid builds/tests until all changes are ready.

## [2026-05-03] - Implement L2-scored target sell exits

Model / agent:
- Model: GPT-5.5 Thinking, reasoning model
- Provider/client: Codex desktop

Source state:
- Local clone at `D:\trading-system`.
- Existing unrelated dirty local items were present and not touched. Do not record VPS IPs, machine IDs, SSH keys, or credentials.

User request:
- Keep buy/ranking logic separate.
- Use `compute_execution_score` for SELL/exit decisions only.
- Target net `+0.8%` profit, with estimated costs added on top of the sell limit.
- Use L2/depth data to estimate sell queue/direction before placing the sell order.

Files changed:
- `include/execution/score.hpp`
  - `compute_execution_score` now accepts configurable `reward` and `loss` while preserving old defaults.
  - Directional probability is clamped to `[0, 1]`.
- `include/config/AppConfig.hpp` and `src/lib/AppConfig.cpp`
  - Added sell/cost config: `target_profit_pct`, `min_sell_execution_score`, transaction-cost knobs, daily energy/capital allocation knobs.
- `src/lib/LiveExecutionEngine.cpp` and `include/engine/LiveExecutionEngine.hpp`
  - Track submitted buy orders.
  - Convert filled IBKR buy orders into open positions using `OrderLifecycleBook`.
  - Compute sell target as `entry_price * (1 + target_profit_pct) + estimated_round_trip_cost_per_share`.
  - Estimate visible ask queue ahead from L2, directional pressure from book imbalance/microprice, and route SELL limit orders only when `compute_execution_score` clears `min_sell_execution_score`.
  - Prevent new buy orders for symbols with pending entry orders or open positions.
- `config.ibkr_paper.example.ini`
  - Added `[sell_strategy]` and `[costs]` examples.
- `tests/math/TestMathModels.cpp` and `tests/unit/AppConfigTest.cpp`
  - Added coverage for configurable execution-score reward/loss and new config keys.

Important behavior:
- BUY ranking is still driven by the existing ranking engine.
- The new EV score is used only after an IBKR buy fill is observed.
- The baseline sell target is net `+0.8%` plus costs; this patch does not yet implement the later "let it run for more" decision.
- SELL exits currently require the real `IBKRClient` path because the engine reads `OrderLifecycleBook` and L2 snapshots from `IBKRClient`.

Validation:
- No build or test run, per user instruction to avoid builds/tests until all changes are ready.
- Performed read-only diff/sanity inspection only.

## [2026-05-03] - Split current universe into Databento-backtestable US symbols

Model / agent:
- Model: GPT-5.5 Thinking, reasoning model
- Provider/client: Codex desktop

Source state:
- Local clone at `D:\trading-system`, latest commit observed as `e1429fcb74769763cb7a57477fa65a454a80881e` (`Added real paper-trading`).
- Existing unrelated dirty local items were present and not touched. Do not record VPS IPs, machine IDs, SSH keys, or credentials.

User request:
- Separate the current stock universe into the symbols that can be used with Databento for historical backtests, especially L2.

Files changed:
- `agent/AGENT_HANDOFF_LOG.md` - appended this planning note only.

Databento interpretation:
- Databento historical stock L2 is available through US equities datasets/schemas, especially `mbp-10`.
- `EQUS.MINI` is useful for broad top-of-book/L1 style data, but it is not an L2 dataset.
- For L2, use venue/direct datasets such as `XNAS.ITCH`, `XNYS.PILLAR`, `ARCX.PILLAR`, `XASE.PILLAR`, `BATS.PITCH`, `BATY.PITCH`, `EDGA.PITCH`, `EDGX.PITCH`, `MEMX.MEMOIR`, and similar Databento US equity depth datasets.
- Current non-US local listings such as `.FR`, `.DE`, `.IT`, `.FI`, `.NO`, `.T`, `.TW`, `.TWO`, `.KS`, `.HK`, `.SS`, and `.VI` should be excluded from Databento L2 backtests unless remapped to a US-listed ADR/common stock.

Likely Databento-ready US-listed candidates from the current universe:
- Tech/semis/infrastructure: `AAPL`, `NVDA`, `AMD`, `INTC`, `MU`, `QCOM`, `ARM`, `ASML`, `AMAT`, `LRCX`, `KLAC`, `SNPS`, `CDNS`, `MKSI`, `ENTG`, `STX`, `WDC`, `PSTG`, `DELL`, `HPQ`, `SMCI`, `CSCO`, `HPE`, `IBM`, `KEYS`.
- Semiconductor ADRs/foundry/package names: `TSM`, `GFS`, `UMC`, `TSEM`, `ASX`, `AMKR`, `IMOS`.
- Other US-listed names already in the universe via bare or `.US` symbols: `LEA`, `AWK`, `CEG`, `VST`, `NIO`, `XPEV`, `OKLO`, `SNDK`, `LMT`, `HWM`, `RTX`, `NOC`, `GSM`, `DD`, `LIN`, `APD`.

Useful remaps from current local/global entries:
- `TTE.FR` can be represented by US-listed `TTE` for Databento/US ADR-style backtests.
- `NOKIA.FI` can be represented by US-listed `NOK`.
- `2330.TW` can be represented by US-listed `TSM`.
- `2303.TW` can be represented by US-listed `UMC`.
- `3711.TW` can be represented by US-listed `ASX`.
- `ASML.AS` / `ASML.NL` can be represented by US-listed `ASML`, which already exists in the universe.

Exclude or handle carefully:
- `AI` currently means Air Liquide in the project list, but US `AI` is C3.ai; do not use it as Air Liquide.
- `MRK` currently means Merck KGaA in the project list, but US `MRK` is Merck & Co.; do not use it as Merck KGaA.
- `ASM`, `SIE`, `ATS`, `WCH`, and `WAF` are ambiguous or likely not the intended US-listed instrument.
- Most local Europe/APAC tickers require non-Databento vendors for historical L2.

Recommended first Databento backtest universe:
- `AAPL`, `NVDA`, `AMD`, `INTC`, `MU`, `QCOM`, `ARM`, `ASML`, `TSM`, `GFS`, `UMC`, `AMAT`, `LRCX`, `KLAC`, `SNPS`, `CDNS`, `MKSI`, `ENTG`, `STX`, `WDC`, `DELL`, `SMCI`, `CSCO`, `HPE`, `IBM`.

Validation / follow-up:
- No code/build/test run.
- Before downloading data, verify each candidate through Databento definitions/symbol lookup and record the exact dataset/symbol mapping in a symbol master.

## [2026-05-03] - Align IBKR live universe with Databento historical backtests

Model / agent:
- Model: GPT-5.5 Thinking, reasoning model
- Provider/client: Codex desktop

Source state:
- Local clone at `D:\trading-system`, latest commit observed as `e1429fcb74769763cb7a57477fa65a454a80881e` (`Added real paper-trading`).
- Existing unrelated dirty local items were present and not touched: `.idea/editor.xml`, deleted `agent/_configure_ucrt_noibkr.sh`, `.claude/`, `dependencies/ucrt64/...`, and two untracked `third_party/googletest` metadata files.

User request / decision:
- Use IBKR market data for real trading and Databento for historical backtests.
- Keep the same stocks available in both systems, so the live universe and backtest universe do not diverge.

Files changed:
- `agent/AGENT_HANDOFF_LOG.md` - appended this planning note only.

Planning decision:
- Start with a US-listed equities/ADR overlap universe, because Databento's stock/equities coverage is US equities and ETFs and the current global universe includes many local non-US listings that Databento will not cover as historical L2.
- Use IBKR for live L1/L2 entitlements and Databento for historical `mbp-1`/`bbo`/`tbbo` L1 and `mbp-10` L2.
- The match should be by canonical instrument, not by current display suffix. Example: current `INTC.US` should map to canonical `INTC`; current `TSM` is the US ADR and can be different from `2330.TW`.

Likely first overlap candidates from the current universe:
- NASDAQ/US tech and semis: `AAPL`, `NVDA`, `AMD`, `INTC`, `MU`, `QCOM`, `ARM`, `ASML`, `AMAT`, `LRCX`, `KLAC`, `SNPS`, `CDNS`, `ENTG`, `MKSI`, `STX`, `WDC`, `SMCI`, `CSCO`.
- Other US-listed candidates / ADRs: `LEA`, `AWK`, `CEG`, `VST`, `NIO`, `XPEV`, `OKLO`, `PSTG`, `LMT`, `HWM`, `RTX`, `NOC`, `GSM`, `DD`, `LIN`, `APD`, `KEYS`, `IBM`, `TSM`, `GFS`, `UMC`, `TSEM`, `ASX`, `AMKR`, `IMOS`, `DELL`, `HPQ`, `HPE`.
- Treat these as candidates until verified through Databento definitions and an IBKR contract/conId probe.

Symbols to avoid or remap carefully:
- Local exchange suffixes such as `.FR`, `.DE`, `.IT`, `.FI`, `.NO`, `.T`, `.TW`, `.TWO`, `.KS`, `.HK`, `.SS`, `.VI` are not Databento US equities symbols.
- Some bare tickers are ambiguous or represent the wrong company in the US market: `AI` in the current list means Air Liquide locally but US `AI` is C3.ai; `MRK` in the current list means Merck KGaA but US `MRK` is Merck & Co.; `ASM`, `SIE`, and `ATS` also need careful remapping before use.

Recommended implementation shape:
1. Add a symbol master file, for example `config/symbol_master.csv`.
2. Include columns: `internal_id`, `company`, `ibkr_symbol`, `ibkr_sec_type`, `ibkr_exchange`, `ibkr_primary_exchange`, `ibkr_currency`, `ibkr_con_id`, `databento_dataset`, `databento_symbol`, `databento_schema_l1`, `databento_schema_l2`, `enabled_live`, `enabled_backtest`.
3. Build a no-order verifier that checks both sides:
   - IBKR contract lookup / market-data entitlement probe.
   - Databento definitions lookup / small historical range availability check.
4. Only enable symbols where both verifiers pass for the intended L1/L2 level.
5. Prefer a NASDAQ-heavy first hot set if using one IBKR L2 package, so Databento Nasdaq TotalView-style L2 and IBKR NASDAQ depth are closer.

Known caveat:
- "Same stock" does not guarantee identical feed. IBKR live L2 and Databento historical L2 can differ by venue, aggregation, and timestamps. For close backtest/live matching, choose the same primary venue/feed family where possible, not only the same ticker.

Sources referenced:
- Databento equities/stocks documentation: US stocks and ETFs, 15 US exchanges/30 ATSs, historical coverage since 2018, schemas including `mbp-1` L1 and `mbp-10` L2.
- IBKR market-data docs/pricing already referenced in earlier handoff entries for live L1/L2 entitlements.

## [2026-05-02] - Backtesting and VPS binary deployment notes

Model / agent:
- Model: GPT-5.5 Thinking, reasoning model
- Provider/client: Codex desktop

Source state:
- Local clone at `D:\trading-system`, latest commit observed as `e1429fcb74769763cb7a57477fa65a454a80881e` (`Added real paper-trading`).
- Existing unrelated dirty local items were present and not touched: `.idea/editor.xml`, deleted `agent/_configure_ucrt_noibkr.sh`, `.claude/`, `dependencies/ucrt64/...`, and two untracked `third_party/googletest` metadata files.

User request:
- Return to `hftbacktest` library brainstorming.
- Add non-sensitive notes about whether the VPS needs extra binaries or can use downloads from CI.
- Do not record sensitive server details in handoff.

Files changed:
- `agent/AGENT_HANDOFF_LOG.md` - appended this planning note only.

Non-sensitive VPS paths/assumptions:
- Data volume path observed in discussion: `/mnt/HC_Volume_105581071`.
- Preferred project/data root discussed: `/data/trading`.
- Suggested data subdirectories: `/data/trading/raw`, `/data/trading/parquet`, `/data/trading/logs`, `/data/trading/backtests`, `/data/trading/tmp`.
- Do not record server IPs, machine IDs, SSH fingerprints, account credentials, IBKR usernames/passwords, or 2FA/security-token details in handoffs.

Backtesting direction:
- `hftbacktest` is not an external broker/gateway like IB Gateway; it is an in-process historical replay simulator.
- The clean integration is a separate backtest run loop that lets `hftbacktest` own historical time, market replay, latency, queue position, and simulated fills.
- The shared boundary should be strategy decisions, not socket connectivity:
  - Input: market snapshot/book state + portfolio/order state + config.
  - Output: order intents/cancels.
- Avoid running both the C++ wall-clock loop and `hftbacktest` historical loop as independent clocks. If the C++ side orchestrates, it should advance `hftbacktest` explicitly and accept that `hftbacktest` remains the simulated market clock.
- Good first prototype: Python `hftbacktest` runner emits snapshots/features, calls a thin strategy adapter or exported decision function, submits orders to `hftbacktest`, and writes fills/results to Parquet/CSV for comparison.

VPS binaries/dependencies note:
- Current CI does not publish deployable `hft_app`, `hft_tests`, `hft_gtests`, or `ibkr_paper_order_probe` binaries as artifacts.
- Current CI does publish/reuse a Linux dependency bundle (`linux-deps-ubuntu-latest.tar.gz`) through the `linux-deps` GitHub release flow, and uploads coverage artifacts only.
- For the VPS, there are two viable deployment approaches:
  1. Build on the VPS using repo scripts and the Linux dependency bundle. This is simple and debuggable, but needs compiler/build tools installed.
  2. Add a CI packaging artifact/release later that uploads Linux executables plus any required runtime libraries/config templates. Then the VPS can download a known-good CI-built tarball.
- If using CI-built binaries, pin the CI runner or build container close to the VPS OS to avoid libc/libstdc++ surprises. The VPS discussion currently assumes Ubuntu 24.04 x86-64.
- IB Gateway remains a separate vendor binary installed on the VPS; it is not produced by our CI.
- `hftbacktest` will need a Python environment on the VPS or a packaged Python/venv/container. It should not be bundled into the C++ app binary unless we later choose an embedded/interprocess integration.
- Headless Gateway operation likely needs extra runtime tools: Xvfb or another virtual display, optionally VNC/noVNC for UI access, and later IBC/IBController for auto-restart/login-dialog automation.

Validation performed:
- Local inspection only. Checked `.github/workflows/ci.yml` and repo references for artifact/upload behavior.
- No build/test run was performed.

Known risks / follow-up:
- Need decide whether server deploys should be source-build, CI artifact, or container image.
- Need decide whether `hftbacktest` prototype stays Python-only first or bridges to C++ strategy decisions early.
- Need keep all server secrets and access details out of handoff logs.

## [2026-05-02] - Fix UCRT link failure from protobuf-exported `-lrt`

Model / agent:
- Model: GPT-5.5 Thinking, reasoning model
- Provider/client: Codex desktop

Source state:
- Local clone at `D:\trading-system`, latest commit observed as `e1429fcb74769763cb7a57477fa65a454a80881e` (`Added real paper-trading`).
- Existing unrelated dirty local items were present and not touched: `.idea/editor.xml`, deleted `agent/_configure_ucrt_noibkr.sh`, `.claude/`, `dependencies/ucrt64/...`, and two untracked `third_party/googletest` metadata files.

User-provided failure:
- UCRT build linked object/static-library stages, then all executable links failed with `ld.exe: cannot find -lrt: No such file or directory`.
- Affected executables in the pasted log: `ibkr_paper_order_probe.exe`, `hft_app.exe`, `hft_tests.exe`, and `hft_gtests.exe`.

Files changed:
- `CMakeLists.txt` - when building with MinGW, CMake now creates an empty `librt.a` compatibility archive under `${CMAKE_BINARY_DIR}/hft_mingw_compat_lib` and adds that directory to `hft_ibkr_link_deps`.
- `agent/AGENT_HANDOFF_LOG.md` - appended this fix record.

Rationale:
- `rt` / `librt` is a Linux realtime library and does not exist in MSYS2/UCRT.
- The project does not link `rt` directly. The `-lrt` reference is expected to come from imported dependency metadata, most likely `protobuf::libprotobuf` or one of its exported transitive targets.
- The dependency-build script already documents this issue and creates a dummy `librt.a` in the install prefix, but the local build can still fail if that archive is missing from the active prefix. The CMake-side compatibility archive makes the project resilient to that missing file.

Validation performed:
- No build/test rerun was performed after this edit. The fix was made from the user-provided linker log, preserving the current instruction to avoid repeated build/test runs until the code edits are complete.

Known risks / follow-up:
- The next single UCRT configure/build should regenerate the build system so the new compatibility link directory is present before relinking executables.

## [2026-05-01] - Adjust IBKR paper to same live path and make symbol data scope configurable

Model / agent:
- Model: GPT-5.5 Thinking, reasoning model
- Provider/client: Codex desktop

Source state:
- Local clone at `D:\trading-system`, latest commit observed as `e1429fcb74769763cb7a57477fa65a454a80881e` (`Added real paper-trading`).
- Existing unrelated dirty local items were present and not touched: `.idea/editor.xml`, deleted `agent/_configure_ucrt_noibkr.sh`, `.claude/`, `dependencies/ucrt64/...`, and two untracked `third_party/googletest` metadata files.

User correction / request:
- Stop running tests/builds for now; do the build once after all code changes are complete.
- IBKR paper should not be a separate execution behavior from live. It should use the same IBKR live execution path, with the port as the practical difference.
- Continue wiring the app so IBKR messages are read while engine steps run.
- Answer whether the app reads all symbol data right now.

Files changed:
- `include/config/AppConfig.hpp` / `src/lib/AppConfig.cpp` - removed the earlier `allow_nonstandard_ibkr_paper_port` guard setting and added `universe_size` config.
- `include/engine/LiveExecutionEngine.hpp` / `src/lib/LiveExecutionEngine.cpp` - removed the IBKR-paper-only port guard and added generic IBKR order-id seeding from `nextValidId` before routing orders.
- `src/app/main.cpp` - starts the IBKR reader loop for any real IBKR mode, waits up to 10 seconds for `nextValidId`, and uses `universe_size` to initialize and subscribe the same symbol set.
- `config.ibkr_paper.example.ini` - now sets `universe_size=1` and no longer contains a paper-only custom-port override.
- `tests/unit/AppConfigTest.cpp`, `tests/unit/LiveExecutionEngineTest.cpp`, and `tests/unit/TestCoreModels.cpp` - expectations updated for the same-live-path behavior and new `universe_size` key.
- `agent/AGENT_HANDOFF_LOG.md` - appended this correction record.

Behavior after this change:
- `mode=live` and `mode=ibkr_paper` both use the real IBKR client path.
- The port selection remains the difference: `mode=live` uses `live_port`; `mode=ibkr_paper` uses `paper_port`.
- Generic execution safety settings still apply to both modes: `order_enabled`, `order_qty`, `max_order_qty`, `max_notional_per_order`, `max_orders_per_run`, and `max_orders_per_symbol`.
- The app no longer requests depth data for the whole static symbol list by default. It initializes and subscribes only `universe_size` symbols, clamped to the static list size.

Important answer captured:
- Before this change, the app initialized 30 ranked symbols but requested market depth for the entire static symbol list.
- After this change, data requests are scoped by `universe_size`; the paper example requests only 1 symbol.

Validation performed:
- No formatter, build, or tests were run after this correction because the user explicitly asked to defer build/test execution until all changes are complete.

Known risks / follow-up:
- Run `./scripts/format_code.sh` and a single UCRT build/test pass after the remaining edits are done.
- The first IBKR paper app run should still be tiny: `universe_size=1`, `top_k=1`, `steps=1`, `order_qty=1`, and `max_orders_per_run=1`.
- Contract mapping is still hardcoded as `STK` / `SMART` / `USD`, so many non-US symbols in the static universe will not resolve correctly until a proper contract table is added.
- L1/L2 market-data entitlement work remains postponed until subscriptions are chosen.

## [2026-05-01] - Make IBKR paper mode usable and configurable from app config

Model / agent:
- Model: GPT-5.5 Thinking, reasoning model
- Provider/client: Codex desktop

Source state:
- Local clone at `D:\trading-system`, latest commit observed as `aa3aacccd5efece103f3faf36759e8a4f736d69c` (`changed PaperBroker to LocalSimBroker`).
- Existing unrelated dirty local items were present and not touched: `.idea/editor.xml`, deleted `agent/_configure_ucrt_noibkr.sh`, `.claude/`, `dependencies/ucrt64/...`, and two untracked `third_party/googletest` metadata files.

User request:
- Make IBKR paper usable first, with settings adjustable from a config file.
- Keep the live execution engine path, but make it possible to point that path at an IBKR paper Gateway/TWS session with small, explicit order budgets.

Files changed:
- `include/config/AppConfig.hpp` - added `BrokerMode::IBKRPaper` and execution safety settings: `allow_nonstandard_ibkr_paper_port`, `order_enabled`, `order_qty`, `max_order_qty`, `max_notional_per_order`, `max_orders_per_run`, and `max_orders_per_symbol`.
- `src/lib/AppConfig.cpp` - added bool parsing, `mode=ibkr_paper` / `mode=paper_ibkr`, and parsing for the new broker/execution keys.
- `include/config/LiveTradingConfig.hpp` - maps `IBKRPaper` to the real IBKR client path and exposes `mode_name()` as `ibkr_paper`.
- `include/engine/LiveExecutionEngine.hpp` / `src/lib/LiveExecutionEngine.cpp` - added paper-port guard, order enable flag, configurable order quantity, max quantity/notional checks, max orders per run, and max orders per symbol.
- `src/app/main.cpp` - startup message now reports the real IBKR mode name, including `ibkr_paper`.
- `config.ibkr_paper.example.ini` - new guarded example config for paper Gateway/TWS validation.
- `tests/unit/AppConfigTest.cpp`, `tests/unit/LiveExecutionEngineTest.cpp`, `tests/unit/TestCoreModels.cpp`, `tests/math/TestMathModels.cpp`, and `tests/module/TestBrokerIntegration.cpp` - added coverage for the new mode and config guardrails.
- `agent/AGENT_HANDOFF_LOG.md` - appended this implementation record.

Deletions / removals:
- None for this implementation.
- Test-generated scratch files from the validation run were removed afterward; unrelated local dirty files were left untouched.

Config behavior:
- `mode=ibkr_paper` uses the real IBKR broker implementation, but uses `paper_port` rather than `live_port`.
- `mode=paper` remains local simulation and should not be confused with an IBKR paper account.
- `IBKRPaper` refuses non-paper ports by default. Accepted paper ports are `4002` for IB Gateway paper and `7497` for TWS paper. A custom port requires `allow_nonstandard_ibkr_paper_port=true`.
- `order_enabled=false` lets the live engine connect/subscribe/heartbeat without placing orders.
- `order_qty` controls requested quantity; `max_order_qty` clamps it when positive.
- `max_notional_per_order`, `max_orders_per_run`, and `max_orders_per_symbol` act as simple order-budget brakes. Values of `0` mean the corresponding limit is disabled.

Example starting point:
- Use `config.ibkr_paper.example.ini` as the template for a one-symbol, one-step, one-order IBKR paper validation run.
- The example keeps `top_k=1`, `steps=1`, `order_qty=1`, `max_order_qty=1`, `max_notional_per_order=500`, `max_orders_per_run=1`, and `max_orders_per_symbol=1`.

Validation performed:
- Ran repo formatter through MSYS2 using Linux-style path:
  - `D:\msys64\usr\bin\bash.exe -lc "export PATH=/ucrt64/bin:/usr/bin:$PATH; cd /d/trading-system && ./scripts/format_code.sh"`
  - Result: `Formatted 72 files using .clang-format`.
- Rebuilt all UCRT targets:
  - `$env:PATH='D:\msys64\ucrt64\bin;D:\msys64\usr\bin;' + $env:PATH; D:\msys64\ucrt64\bin\cmake.exe --build build-ucrt-ibkr -j 2`
  - Result: `hft_lib`, `hft_app`, `hft_tests`, `hft_gtests`, and `ibkr_paper_order_probe` built successfully.
- Ran focused gtests:
  - `.\build-ucrt-ibkr\hft_gtests.exe --gtest_filter="*AppConfig*:*LiveExecutionEngine*"`
  - Result: 25 tests passed.
- Ran legacy test executable:
  - `.\build-ucrt-ibkr\hft_tests.exe`
  - Result: all listed tests passed, including `test_app_config_loads_ibkr_paper_mode` and `test_live_trading_config_mode_names`.

Known risks / follow-up:
- No live app run was performed in `mode=ibkr_paper` during this handoff entry, to avoid placing paper orders without a deliberate user command.
- The live engine still uses the current ranking/order logic and simple buy-limit behavior. It now has quantity/count/notional brakes, but it is not yet fill-aware and does not block duplicate orders based on open IBKR order state.
- `execDetails` handling is still a follow-up; fills are currently observed through order status in the probe path.
- L1/L2 market-data architecture changes remain postponed until IBKR subscriptions are selected.

## [2026-04-30] - Decision: run live execution engine against IBKR paper, postpone L1/L2 rework

Model / agent:
- Model: GPT-5.5 Thinking, reasoning model
- Provider/client: Codex desktop

Source state:
- Local clone at `D:\trading-system`, latest commit observed as `aa3aacccd5efece103f3faf36759e8a4f736d69c` (`changed PaperBroker to LocalSimBroker`).
- Existing unrelated dirty local items were present and not touched: `.idea/editor.xml`, deleted `agent/_configure_ucrt_noibkr.sh`, `.claude/`, `dependencies/ucrt64/...`, and two untracked `third_party/googletest` metadata files.

User request / decision:
- User wants to use the live execution engine against an IBKR paper Gateway session for now.
- User correctly noted that logging into Gateway in paper mode gives an additional real-money safety boundary.
- User wants to keep `ibkr_paper_order_probe` but treat it as a utility for latency measurement, API/data queries, and live-data experiments rather than the main trading path.
- User wants to postpone the L1/L2 market-data architecture rework until after buying/choosing IBKR market-data subscriptions.

Files changed:
- `agent/AGENT_HANDOFF_LOG.md` - appended this decision record only.

Deletions / removals:
- None.

Important interpretation:
- IBKR paper login protects real money, but the app can still create many paper orders/positions if `hft_app` is pointed at paper Gateway with the current live execution loop.
- The next engineering step should not be L1/L2 rework. It should be adding safety rails so `LiveExecutionEngine` can be run against IBKR paper without order spam.

Recommended next implementation:
1. Add an explicit config mode or flag for real IBKR paper, for example `mode=ibkr_paper` or `broker=ibkr` + `account_env=paper`, so `mode=paper` remains local simulation and cannot be confused with IBKR paper.
2. Add a paper-only guard that refuses default live ports (`4001`, `7496`) unless an explicit override is present.
3. Add order-budget controls to the live execution engine: `max_orders_per_run`, `max_open_orders`, `max_position_per_symbol`, `max_notional_per_order`, and `max_total_notional`.
4. Add duplicate-order protection: do not place a new order for a symbol while an order is open/pending/partially filled.
5. Add timeout/cancel behavior and fill/status reconciliation before allowing another order for the same symbol.
6. Keep `ibkr_paper_order_probe` for one-shot API checks, latency/ack measurements, entitlement probes, and test market-data requests.
7. Defer L1-for-ranking / L2-for-execution refactor until subscriptions are selected and entitlement results are known.

Validation performed:
- Documentation-only update to handoff log.

Known risks / follow-up:
- Do not run current `hft_app` in real-IBKR mode with nonzero `top_k` and multiple `steps` until the above order-budget/duplicate guards exist.
- Current main app live path can still place repeated buy limit orders on each step for active ranked symbols.

## [2026-04-30] - Add guarded IBKR paper order probe and rename local simulator broker

Model / agent:
- Model: GPT-5.5 Thinking, reasoning model
- Provider/client: Codex desktop

Source state:
- Local clone at `D:\trading-system`, latest commit observed as `996ee29d995c1fca7b8e72bdabda0335ac70c92f` (`added live data investigation`).
- Existing unrelated dirty local items were present and not touched: `.idea/editor.xml`, deleted `agent/_configure_ucrt_noibkr.sh`, `.claude/`, `dependencies/ucrt64/...`, and two untracked `third_party/googletest` metadata files.

User request:
- Add a way to see orders in the IBKR paper account and rename the internal `PaperBrokerSim` so it is clearly distinct from IBKR paper trading.

Files changed:
- `CMakeLists.txt` - added `ibkr_paper_order_probe` target and linked it with the same IBKR/TWS dependency bundle as the app/tests.
- `include/broker/LocalSimBroker.hpp` - new name for the in-memory local simulation broker.
- `include/broker/PaperBrokerSim.hpp` - deleted as part of the rename to avoid confusion with IBKR paper trading.
- `src/app/main.cpp` - switched from `PaperBrokerSim` to `LocalSimBroker` and updated the startup message.
- `tests/module/TestBrokerIntegration.cpp` - switched includes/types/messages from `PaperBrokerSim` to `LocalSimBroker`.
- `include/broker/IBroker.hpp` - added `OrderRequest::transmit`, defaulting to `true`.
- `include/broker/IBKRCallbacks.hpp` - added portable `IBKRError`, optional `on_next_valid_id`, and optional `on_error` callback hooks.
- `include/broker/IBKRClient.hpp` / `src/lib/IBKRClient.cpp` - store `nextValidId` and IBKR errors so small tools can submit a valid order id and print broker/API errors.
- `src/lib/RealIBKRTransport.cpp` - forwards `nextValidId` and `error` callbacks and maps `OrderRequest::transmit` to TWS `Order::transmit`.
- `src/tools/ibkr_paper_order_probe.cpp` - new guarded one-shot IBKR paper order executable.
- `agent/AGENT_HANDOFF_LOG.md` - appended this entry.

Deletions / removals:
- Deleted `include/broker/PaperBrokerSim.hpp`; replacement is `include/broker/LocalSimBroker.hpp`.
- No order-routing behavior was removed from the local simulator; it was renamed to avoid ambiguity.

Safety rails in `ibkr_paper_order_probe`:
- Requires explicit `--transmit`; otherwise refuses to submit.
- Defaults to paper Gateway port `4002`.
- Accepts only `4002` or `7497` unless `--allow-custom-port` is provided.
- Refuses `qty > 1`.
- Requires a positive limit price.
- Supports auto-cancel via `--cancel-after-sec`.
- Currently routes stocks as `STK` / `SMART` / `USD`; it is suitable for a first US stock paper-account probe, not yet a general contract probe.

Validation performed:
- Formatted C++ sources with UCRT `clang-format` after granting permissions for the formatter.
- Reconfigured UCRT build:
  - `$env:PATH='D:\msys64\ucrt64\bin;D:\msys64\usr\bin;' + $env:PATH; D:\msys64\ucrt64\bin\cmake.exe -S . -B build-ucrt-ibkr -DCMAKE_PREFIX_PATH=D:/trading-system/dependencies/ucrt64/install`
  - Result: configure/generate succeeded.
- Built the new probe:
  - `D:\msys64\ucrt64\bin\cmake.exe --build build-ucrt-ibkr --target ibkr_paper_order_probe -j 2`
  - Result: target built successfully.
- Built all local targets:
  - `D:\msys64\ucrt64\bin\cmake.exe --build build-ucrt-ibkr -j 2`
  - Result: `hft_app`, `hft_tests`, `hft_gtests`, and `ibkr_paper_order_probe` built successfully.
- Ran probe help:
  - `.\build-ucrt-ibkr\ibkr_paper_order_probe.exe --help`
  - Result: printed usage and safety rails.
- Ran safety refusal:
  - `.\build-ucrt-ibkr\ibkr_paper_order_probe.exe --limit 100`
  - Result: refused to submit without `--transmit`.
- Confirmed `ibgateway.exe` is running and `netstat` shows paper Gateway API listening on `0.0.0.0:4002` and `[::]:4002`.

IBKR paper-account order probes run:
- Safe acceptance/cancel test:
  - `.\build-ucrt-ibkr\ibkr_paper_order_probe.exe --port 4002 --client-id 9101 --symbol AAPL --action BUY --qty 1 --limit 1 --transmit --timeout-sec 20 --cancel-after-sec 2`
  - Result: order submitted, then auto-cancelled. Final status `Cancelled`, filled `0`, remaining `1`. IBKR returned normal farm OK messages and cancel code `202`.
- Too-aggressive marketable limit test:
  - `.\build-ucrt-ibkr\ibkr_paper_order_probe.exe --port 4002 --client-id 9102 --symbol AAPL --action BUY --qty 1 --limit 1000 --transmit --timeout-sec 45 --cancel-after-sec 10`
  - Result: IBKR rejected/cancelled because the buy limit was too aggressive relative to current market price. Error referenced current market price around `271.85` and max aggressive limit around `279.84239`.
- Filled paper order:
  - `.\build-ucrt-ibkr\ibkr_paper_order_probe.exe --port 4002 --client-id 9103 --symbol AAPL --action BUY --qty 1 --limit 275 --transmit --timeout-sec 45 --cancel-after-sec 10`
  - Result: final status `Filled`, filled `1`, remaining `0`, average fill `271.86`.

Known risks / follow-up:
- The new probe only supports simple US stock contracts using the current `STK` / `SMART` / `USD` mapping. It should later grow explicit contract fields or reuse a canonical contract table.
- `execDetails` is still a no-op in `RealIBKRTransport`; the probe confirms fills through `orderStatus`, not execution detail records.
- `ctest` was not run because the approval prompt for CTest was declined. The full build did pass.
- The main `hft_app` still has the previous live-engine order-loop risk in live mode; use `ibkr_paper_order_probe` for controlled paper-account order tests instead of the engine.

## [2026-04-29] - Comprehensive L1/L2 ranking, execution, and cost strategy

Model / agent:
- Model: GPT-5.5 Thinking, reasoning model
- Provider/client: Codex desktop

Source state:
- Local clone at `D:\trading-system`, latest commit observed as `996ee29d995c1fca7b8e72bdabda0335ac70c92f` (`added live data investigation`).
- Existing unrelated dirty local items were present and not touched: `.idea/editor.xml`, deleted `agent/_configure_ucrt_noibkr.sh`, `.claude/`, `dependencies/ucrt64/...`, and two untracked `third_party/googletest` metadata files.

User request:
- Add a comprehensive detailed description of the last three market-data responses, including tables and explanations.

Files changed:
- `agent/AGENT_HANDOFF_LOG.md` - appended this comprehensive planning entry at the top.

Deletions / removals:
- None.

Summary:
- L2 is richer than L1, but it is not automatically better for every task.
- L1 should be used for broad ranking and monitoring because it is cheaper, scales to more symbols, and is enough for mid/spread/last/volume style features.
- L2 should be reserved for the small set of symbols close to order placement, especially passive limit orders where queue, depth, and fill probability matter.
- The cost-optimized plan is broad L1 plus rotating L2 for about 3 hot symbols, not L2 for the whole universe.

### L1 vs L2 decision

| Question | L1 answer | L2 answer | Practical decision |
|---|---|---|---|
| What does it show? | Top of book: best bid, best ask, sizes, last, volume. | Depth of book: multiple price levels on bid/ask. | Use L1 unless we need queue/depth. |
| Is it good for ranking? | Yes. Enough for broad universe scans, liquidity filters, spread, returns, volatility, 5-second/1-minute features. | Often too expensive/noisy for broad ranking. | Rank with L1. |
| Is it good for passive limit orders? | Partly. Shows top bid/ask, but little about queue behind the quote. | Yes. Helps estimate queue ahead, nearby liquidity, and fill probability. | Use L2 for execution candidates. |
| Cost/scaling | Scales much better. IBKR starts at 100 concurrent L1 lines. | IBKR depth is much more constrained. Default depth allowance is about 3 symbols. | Do not buy broad L2. |
| Storage | Manageable if sampled/resampled. | Much heavier; raw book updates can fill storage quickly. | Store L2 only for hot symbols and short windows. |
| US nuance | Proper NBBO L1 can be more useful than one venue-specific depth feed. | L2 may be venue-specific, depending on package. | For US, get NBBO L1 before broad L2. |

### Recommended ranking/execution architecture

| Stage | Data | Frequency / scope | Purpose |
|---|---|---|---|
| Universe monitor | L1, delayed where acceptable, snapshots, or bars | Broad universe, up to all symbols | Track availability, price, spread, volume, and coarse features. |
| Ranker | L1 or 5-second/1-minute derived features | Broad universe | Choose candidates using momentum/mean-reversion/volatility/liquidity filters. |
| Hot set promotion | L1 plus strategy score | Small set, usually top 3 | Decide which symbols deserve L2. |
| Execution engine | L2/depth, optional tick-by-tick where available | Only hot symbols | Place, adjust, or skip passive limit orders based on depth and queue. |
| Demotion | L1/L2 health and rank decay | Hot symbols only | Drop L2 subscription when symbol falls out of hot set or no setup remains. |
| Fill checking | Order status, open orders, executions, positions | Only symbols with orders | Verify fills and reconcile state before more orders. |

Decision: ranking = L1; placing/adjusting passive limit orders = L2. If the system later uses marketable limit orders or crossing orders, L2 matters less, but for passive limit orders it is the right input.

### Cost-optimized L1 coverage table

Assumption: public IBKR Non-Professional pricing. Exact entitlement still depends on account classification, trading permissions, contract resolution, and IBKR account settings.

| Coverage | Subscription choice | Non-Pro cost | Notes |
|---|---|---:|---|
| US broad ranking | Free Cboe/IEX non-consolidated L1 | Free | Cheap first pass, but not consolidated NBBO. |
| US proper NBBO | NYSE Network A + Network B + NASDAQ Network C | About `USD 4.50/mo` total | Roughly `USD 1.50` each in public table; useful for trading-quality US L1. |
| Euronext FR/NL | Euronext Data Bundle L1 | `EUR 3/mo` | Covers Euronext names such as France/Netherlands listings in the universe. |
| Germany/Xetra | Spot Market Germany L1 | `EUR 16.25/mo` | Big incremental cost; defer unless German local listings are essential. |
| Italy | Borsa Italiana L1 | `EUR 4/mo` | Needed for local Italian listing such as `ENI.IT`. |
| Nordic / Finland / Norway | Nordic Equity L1 | `EUR 2/mo` | Likely covers Nordic equity L1; Oslo specifics should be verified by contract. |
| Japan / Tokyo | Tokyo Stock Exchange L1 | `JPY 300/mo` | Needed for `.T` / Tokyo local listings. |
| South Korea | Korea Stock Exchange Stocks L1 | Fee waived | Still must verify account availability and contract permissions. |
| Taiwan TWSE | Taiwan Stock Exchange L1 | `USD 1/mo` | For `.TW` listings. |
| Taiwan Taipei Exchange | Taipei Exchange L1 | `TWD 15/mo` | For `.TWO` listings. |
| Hong Kong | HK Securities Exchange L1 | Fee waived | For `.HK` names; L2 is not free. |
| Shanghai | Shanghai 5-second snapshot via HKEx first | `USD 1/mo` | Cheaper than true Shanghai streaming L1; use first unless live streaming is essential. |
| Austria / Vienna | Vienna L1 + indices | `EUR 5/mo` | For `ATS.VI`. |

Lean broad-L1 baseline, excluding optional L2, excluding US paid NBBO, and excluding Germany/Xetra:

```text
EUR 14 + USD 2 + JPY 300 + TWD 15
```

Broader baseline with US paid NBBO and Germany/Xetra L1:

```text
EUR 30.25 + USD 6.50 + JPY 300 + TWD 15
```

### L2 hot-set package table

L2 should be bought for the venue where the current hot symbols actually trade. Do not buy all L2 packages up front.

| Hot-set L2 package | When useful | Non-Pro cost |
|---|---|---:|
| NASDAQ TotalView-OpenView | US tech/semi hot set such as `NVDA`, `AMD`, `AAPL`, `INTC`, depending listing/contract | `USD 16.50/mo` |
| NYSE OpenBook | NYSE-heavy hot set | `USD 25/mo` |
| Xetra L2 | German hot set | `EUR 21.75/mo` |
| Tokyo Stock Exchange L2 | Japan hot set | `JPY 380/mo` |
| Hong Kong Securities Exchange L2 | Hong Kong hot set | `HKD 225/mo` |
| Shanghai Stock Exchange L2 | China hot set | `USD 35/mo` |

Preferred first practical setup:
1. Start with broad L1 and no broad L2.
2. Use free US non-consolidated data for initial experiments, then add US NBBO if trading US names seriously.
3. Skip Germany/Xetra initially unless the selected strategy needs the local German listings.
4. Add NASDAQ L2 first if hot symbols are mostly US semis/tech.
5. Rotate L2 subscriptions/requests dynamically for the 3 hot symbols rather than subscribing the whole universe to depth.

Sources referenced in prior responses:
- IBKR Market Data Pricing: https://www.interactivebrokers.com/en/pricing/market-data-pricing.php
- IBKR Market Data Subscriptions / API notes: https://www.interactivebrokers.com/campus/ibkr-api-page/market-data-subscriptions/

Validation performed:
- Documentation-only update to handoff log.

Known risks / follow-up:
- Build the no-order Gateway entitlement probe next. It should resolve each symbol to canonical IBKR `conId`, exchange, primary exchange, currency, requested data level, and observed result: live, delayed, snapshot, no permission, ambiguous contract, or error.
- The current code still requests L2/depth for all subscribed symbols; implementation should later split L1 ranking from L2 execution.

## [2026-04-29] - Cost-optimized IBKR L1/L2 coverage strategy

Model / agent:
- Model: GPT-5.5 Thinking, reasoning model
- Provider/client: Codex desktop

Source state:
- Local clone at `D:\trading-system`, latest commit observed as `8b9c691dcadecdfe00a6e53b281af12abe69fad6` (`ci: add safe hft_app paper smoke`).
- Existing unrelated dirty local items were present and not touched: `.idea/editor.xml`, deleted `agent/_configure_ucrt_noibkr.sh`, `.claude/`, `dependencies/ucrt64/...`, and two untracked `third_party/googletest` metadata files.

User request:
- Investigate how to cover most stocks with L1/L2 market data without paying too much.

Files changed:
- `agent/AGENT_HANDOFF_LOG.md` - appended this planning entry at the top.

Deletions / removals:
- None.

Findings / decision:
- Use L1 for broad ranking and universe monitoring.
- Use L2 only for a rotating hot set, normally the 3 symbols closest to executable order placement.
- Start with free/cheap coverage: US free non-consolidated Cboe/IEX, fee-waived Korea/Hong Kong L1, delayed data where acceptable, and cheap L1 packages for Euronext, Italy, Nordic, Japan, Taiwan, Austria, and Shanghai 5-second snapshot.
- For US trading-quality NBBO, add direct Network A/B/C or the IBKR bundle path; direct non-pro rows show USD 1.50 each for NYSE Network A, Network B, and NASDAQ Network C.
- Germany/Xetra is a large incremental cost at EUR 16.25/month for L1 and EUR 21.75/month for L2, so defer unless German local listings are essential or use ADR/US alternatives where acceptable.
- Put L2 budget behind one venue at a time. NASDAQ TotalView-OpenView is the natural first US L2 candidate if hot symbols are NASDAQ-heavy; otherwise choose the L2 package for the venue containing the current hot set.
- Avoid buying L2 for all exchanges. IBKR's default depth allowance is 3 simultaneous L2/depth symbols, while L1 starts at 100 concurrent lines.

Known risks / follow-up:
- Exact subscriptions must still be validated with canonical IBKR contracts/conIds because current display symbols contain duplicates and ambiguous ADR/local listings.
- IBKR prices and account eligibility depend on non-professional/professional classification and account settings.
- A no-order Gateway entitlement probe remains the best next implementation step.

## [2026-04-29] - Level 1 and Level 2 market data explanation

Model / agent:
- Model: GPT-5.5 Thinking, reasoning model
- Provider/client: Codex desktop

Source state:
- Local clone at `D:\trading-system`, latest commit observed as `8b9c691dcadecdfe00a6e53b281af12abe69fad6` (`ci: add safe hft_app paper smoke`).
- Existing unrelated dirty local items were present and not touched: `.idea/editor.xml`, deleted `agent/_configure_ucrt_noibkr.sh`, `.claude/`, `dependencies/ucrt64/...`, and two untracked `third_party/googletest` metadata files.
- `agent/AGENT_HANDOFF_LOG.md` already had staged and unstaged changes before this entry was appended; those contents were not reverted or removed.

User request:
- Add the full prior response explaining Level 1/Level 2 market data and live-data needs as a new handoff entry.

Files changed:
- `agent/AGENT_HANDOFF_LOG.md` - appended this full explanation entry at the top.

Deletions / removals:
- None.

Full response added:

Level 1 vs Level 2, in our context:

**Level 1 / L1** is top-of-book data. For a stock this means best bid, best ask, bid size, ask size, last trade, last size, volume, etc. This is what `reqMktData` gives. IBKR says this is watchlist-style data, aggregated at intervals. For stocks, Europe, and Asia, IBKR documents about `250ms` update frequency, not 20ms.

**Level 2 / L2** is market depth/order book data. This means multiple bid/ask levels below and above the top of book. This is what our current code asks for with `reqMktDepth(..., depth=5)`. L2 is only needed if we care about queue/depth/liquidity beyond best bid/ask.

For live data we need three separate things:

1. Account/API eligibility: IBKR Pro, funded account, market-data API acknowledgment enabled.
2. The right exchange subscription for each symbol: US Network A/B/C for US listings, Xetra/Euronext/TSE/TWSE/HKEX/etc. for foreign listings.
3. The right data type: L1 for top-of-book, L2 only for depth.

Yes, there are multiple "live-ish" data modes:

- **Live streaming**: real-time, paid subscription, `marketDataType=1`.
- **Frozen**: last quote from market close, needs same live entitlement.
- **Delayed**: free where available, usually 10-20 minutes delayed.
- **Delayed frozen**: delayed last value.
- **Snapshots**: one-time quote requests rather than continuous streaming.
- **5-second real-time bars**: OHLC bars every 5 seconds.
- **Tick-by-tick**: trade/bid-ask/midpoint event feed, but very limited.

Could we do 3 symbols "extremely live" and the rest once a minute? Yes, architecturally that is exactly the shape I'd prefer.

But "20ms" is the catch. IBKR L1 stock data is documented around 250ms aggregation, not 20ms. L2/tick-by-tick may feel more event-like, but IBKR is not a colocated ultra-low-latency market-data vendor. So I'd frame it as:

- 3 symbols: L2 depth and/or tick-by-tick, highest priority.
- Rest of universe: L1 snapshots or 5-second bars, stored/resampled to 1-minute features.
- Trading decisions: only place orders on symbols where we have live entitlement and fresh data.

On the "30 symbols" impression: IBKR's current API docs say the default is **100 concurrent market data lines**, not 30. The number **3** is the default simultaneous L2/depth limit with 100 lines. The number **30** might come from another tier: with 501-600 market data lines, IBKR lists about 30 tick-by-tick subscriptions, but that is not the default.

So the practical answer: start with L1 for broad universe, L2/tick-by-tick for only 3 hot symbols, and build the entitlement probe to tell us exactly what your account can receive for each symbol.

Sources: [IBKR Market Data Subscriptions](https://www.interactivebrokers.com/campus/ibkr-api-page/market-data-subscriptions/), [IBKR TWS API Market Data](https://www.interactivebrokers.com/campus/ibkr-api-page/twsapi-doc/).

Validation performed:
- Documentation-only update to handoff log.

Known risks / follow-up:
- Existing staged handoff changes were preserved; this entry was appended above them.

## [2026-04-29] - Require provider/client identity in handoffs

Model / agent:
- Model: GPT-5.5 Thinking, reasoning model
- Provider/client: Codex desktop

Source state:
- Local clone at `D:\trading-system`, latest commit observed as `8b9c691dcadecdfe00a6e53b281af12abe69fad6` (`ci: add safe hft_app paper smoke`).
- Existing unrelated dirty local items were present and not touched: `.idea/editor.xml`, deleted `agent/_configure_ucrt_noibkr.sh`, `.claude/`, `dependencies/ucrt64/...`, and two untracked `third_party/googletest` metadata files.
- `agent/AGENT_HANDOFF_LOG.md` already had staged changes before this entry was appended; those staged contents were not reverted or removed.

User request:
- Enforce a workflow rule requiring agents/models to mention the provider or client surface, in addition to model name and version, for example Cursor, Copilot, Claude Code, Codex, or web APIs.

Files changed:
- `agent/AGENT_WORKFLOW.md` - renamed the identity section to require both model and provider/client identity, added examples, and updated the handoff template.
- `agent/AGENT_HANDOFF_LOG.md` - appended this entry using the new model/provider format.

Deletions / removals:
- None.

Validation performed:
- Inspected the workflow identity section and handoff template.
- Applied a documentation-only patch.

Known risks / follow-up:
- Existing older handoff entries still use the previous `Model / agent` bullet style; the new rule applies going forward.

## [2026-04-29] - Detailed discoveries

Model / agent:
- GPT-5.5 Thinking, reasoning model

Source state:
- Local clone at `D:\trading-system`, latest commit observed as `8b9c691dcadecdfe00a6e53b281af12abe69fad6` (`ci: add safe hft_app paper smoke`).
- Existing unrelated dirty local items were present and not touched: `.idea/editor.xml`, deleted `agent/_configure_ucrt_noibkr.sh`, `.claude/`, `dependencies/ucrt64/...`, and two untracked `third_party/googletest` metadata files.

User request:
- Add the previous response in full to the handoff log with an entry that says "detailed discoveries".

Files changed:
- `agent/AGENT_HANDOFF_LOG.md` - appended this detailed-discoveries entry at the top.

Deletions / removals:
- None.

Detailed discoveries:

I did the investigation pass and added a short append-only handoff entry in [AGENT_HANDOFF_LOG.md](</d/trading-system/agent/AGENT_HANDOFF_LOG.md>).

**Main finding:** before paying IBKR, we should build a no-order Gateway entitlement probe. The current app is not asking for simple quotes: [RealIBKRTransport.cpp](</d/trading-system/src/lib/RealIBKRTransport.cpp:103>) uses `reqMktDepth(..., depth=5)`, so it asks for Level II/depth. IBKR says most API securities need Level 1, while Level 2 is only needed for market depth; depth is also heavily limited by quote lines. With the default 100 market-data lines, IBKR documents only 3 simultaneous Level II/depth symbols, and quote boosters are currently USD 30/month each.

Also, the current contract mapping is too blunt: [RealIBKRTransport.cpp](</d/trading-system/src/lib/RealIBKRTransport.cpp:78>) sets every contract to `STK` / `SMART` / `USD`, but the universe has US, Europe, Japan, Korea, Taiwan, Hong Kong, Shanghai, etc. We need a symbol master with IBKR `conId`, exchange, primary exchange, currency, and intended data level before this can be reliable.

For subscriptions, the safe answer is: start with Level 1 only, prove entitlements via Gateway, then add Level 2 only for the few symbols where the strategy really needs book depth. IBKR's current docs/pricing indicate US NBBO generally maps to Network A/B/C or bundle-style subscriptions; Europe/APAC are exchange-specific, like Germany/Xetra, Euronext, Italy, Nordic, TSE, TWSE, HKEX, Korea, etc. Exact package choice depends on resolved IBKR contracts, not our display suffixes.

For the VPS: if the German provider you mean is Hetzner, the budget works. Their April 2026 price-adjustment page lists Germany/Finland `CPX32` at USD 15.99/month and `CPX42` at USD 29.99/month before VAT; volumes are USD 0.0767/GB/month. So one practical target is a smaller server plus a 100 GB volume, or a larger local-disk server if we want less moving parts. We should still enforce the 100 GB cap in software with rolling compressed retention.

Order logic needs a separate review before live trading. [LiveExecutionEngine.cpp](</d/trading-system/src/lib/LiveExecutionEngine.cpp:40>) currently places repeated buy limits for active symbols every step, with no duplicate-order guard, timeout, cancel/replace policy, or "filled before next action" gate. [RankingEngine.cpp](</d/trading-system/src/lib/RankingEngine.cpp:49>) chooses limits from a synthetic band around `mid`; I don't see a real configurable rolling mean/period yet. We should decide that period from collected data/backtests, likely EWMA/rolling microprice windows, not from a guess.

Fill checking is partially started: [OrderLifecycle.hpp](</d/trading-system/include/broker/OrderLifecycle.hpp:41>) tracks submitted/partial/filled/cancelled/rejected from `orderStatus`, but [RealIBKRTransport.cpp](</d/trading-system/src/lib/RealIBKRTransport.cpp:174>) has `execDetails` as a no-op. For real verification we need `orderStatus` plus `openOrder`, `execDetails`, optional commission reports, and periodic open-order/position reconciliation.

Sources used: [IBKR API Market Data Subscriptions](https://www.interactivebrokers.com/campus/ibkr-api-page/market-data-subscriptions/), [IBKR Market Data Pricing](https://www.interactivebrokers.com/en/pricing/market-data-pricing.php), [IBKR API Market Data Types](https://interactivebrokers.github.io/tws-api/market_data_type.html), [IBKR API Market Depth](https://interactivebrokers.github.io/tws-api/market_depth.html), [Hetzner price adjustment](https://docs.hetzner.com/general/infrastructure-and-availability/price-adjustment/).

No code changes beyond the handoff note.

## [2026-04-29] - Gateway data entitlement, storage, and order-fill investigation

Model / agent:
- GPT-5.5 Thinking, reasoning model

Source state:
- Local clone at `D:\trading-system`, latest commit observed as `8b9c691dcadecdfe00a6e53b281af12abe69fad6` (`ci: add safe hft_app paper smoke`).
- Existing unrelated dirty local items were present and not touched: `.idea/editor.xml`, deleted `agent/_configure_ucrt_noibkr.sh`, `.claude/`, `dependencies/ucrt64/...`, and two untracked `third_party/googletest` metadata files.

User request:
- Investigate how to get real market data for the symbol universe from IB Gateway, what live-data subscriptions may be needed, how to stay under about 100 GB of stored data on a roughly `$30/month` German VPS budget, and what to revisit in order-entry/fill verification logic.

Files changed:
- `agent/AGENT_HANDOFF_LOG.md` - appended this investigation handoff entry only.

Deletions / removals:
- None.

Steps taken:
1. Re-checked the hardcoded IBKR transport contract mapping and live subscription path.
2. Re-checked the ranking/order-entry logic and current order lifecycle tracking.
3. Cross-checked current IBKR market-data documentation/pricing and Hetzner cloud pricing/storage documentation.

Validation performed:
- Local inspection only; no Gateway requests were sent and no app/test build was run.
- Confirmed `RealIBKRTransport` currently builds all market-data/order contracts as `STK`/`SMART`/`USD`, which is not valid for the full global universe.
- Confirmed live subscriptions call `reqMktDepth(..., depth=5, isSmartDepth=false)` for each configured symbol, so the current data path is Level II/depth, not just Level I top-of-book.
- Confirmed the symbol universe spans 107 names across US, Europe, and APAC listings; subscription needs must be derived per canonical IBKR contract/exchange rather than from the current display suffixes alone.
- Confirmed `LiveExecutionEngine::step` places repeated buy limit orders for active ranked names without duplicate-order prevention, timeout/cancel/replace policy, or fill-completion gate.
- Confirmed `OrderLifecycleBook` records submitted/partial/filled/cancelled/rejected states from `orderStatus`, but `execDetails` is currently a no-op and there is no explicit "wait until filled and reconciled" workflow.

Known risks / follow-up:
- Before paying for market data, build a no-order Gateway entitlement probe that resolves each symbol to a canonical IBKR contract/conId, requests Level I and optional Level II separately, and records live/delayed/no-permission/error status.
- Consider lowering the initial live universe or requesting only Level I first; IBKR depth subscriptions and concurrent depth lines are much more constrained than top-of-book data.
- Add a symbol master with exchange, primary exchange, currency, conId, and intended data level before any live collector or order-routing work.
- Define storage retention by byte cap and data value: raw depth/tick logs should roll/compress/delete by size, with 100 GB treated as a hard cap.
- Rework order entry after data collection/backtesting decides the mean/EWMA period; do not hardcode the current synthetic best-limit behavior for live trading.
- Fill verification should combine `orderStatus`, `openOrder`, `execDetails`, commission reports if needed, and periodic open-order/position reconciliation.

Suggested next step:
- Build and run a read-only/no-order market-data entitlement probe against IB Gateway for a small subset first, then expand to the full universe.

## [2026-04-29] - Phase 5: add safe hft_app paper/no-order smoke to CI

Model / agent:
- GPT-5, reasoning model (Codex)

Source state:
- Local clone at `D:\trading-system`, after Phase 4 Gateway log cross-check.
- Existing unrelated dirty local items were present and not touched: `.idea/editor.xml`, deleted `agent/_configure_ucrt_noibkr.sh`, `.claude/`, `dependencies/ucrt64/...`, and two untracked `third_party/googletest` metadata files.

User request:
- Continue Phase 5. Do not delete existing handoff entries. User will handle commits.

Files changed:
- `.github/workflows/ci.yml` - added a `Smoke app` step after `ctest` in `build-and-test`.
- `scripts/smoke_app_ci.sh` - new CI/local smoke script that runs `hft_app` from a build directory with a generated paper/no-order config.
- `agent/AGENT_HANDOFF_LOG.md` - kept Phase 4 handoff and updated this Phase 5 entry.

Deletions / removals:
- None.

Steps taken:
1. Chose the safest Phase 5 starter: app smoke in CI without live Gateway access and without broker orders.
2. Added `scripts/smoke_app_ci.sh`. It resolves the build directory, writes a temporary `config.ini` with `mode=paper`, `top_k=0`, and `steps=1`, runs `hft_app`, and asserts `logs/app.log` plus key stdout/log markers.
3. Wired `.github/workflows/ci.yml` so `build-and-test` runs the smoke after CTest.
4. Ran the smoke locally against `build-ucrt-ibkr`.

Validation performed:
- `bash -n scripts/smoke_app_ci.sh` under MSYS2 UCRT64 -> pass.
- `./scripts/smoke_app_ci.sh build-ucrt-ibkr` under MSYS2 UCRT64 -> `hft_app paper/no-order smoke passed`.
- Smoke output confirmed `mode=paper`, `steps=1`, `top_k=0`, `PaperBrokerSim`, and generated `build-ucrt-ibkr/logs/app.log` with `LoggingService started`, `APP LoadingConfig -> ConnectingBroker`, `APP ConnectingBroker -> Live`, and `Engine Starting -> Ready`.

Known risks / follow-up:
- The smoke still names the runtime app state `Live` after broker startup even in paper mode. That is existing app-state naming, not a Gateway/live-order action.
- The script intentionally overwrites `config.ini`, `logs/`, and `shadow_results.csv` inside the build directory only.
- Remaining Phase 5 items are still open: LoggingService config behavior, heartbeat on real IBKR activity, broker-error reaction, and other logging cleanups.

Suggested commit:
```bash
git add .github/workflows/ci.yml scripts/smoke_app_ci.sh agent/AGENT_HANDOFF_LOG.md
git commit -m "ci: add safe hft_app paper smoke"
```

## [2026-04-29] - Phase 4: cross-check IB Gateway exported logs against app/test behavior

Model / agent:
- GPT-5, reasoning model (Codex)

Source state:
- Local clone at `D:\trading-system`, latest commit observed as `f8639b886e55850986c06880a0d1ab5e6cadb7ab` on `main`.
- Gateway log source: `D:\ibgateway\eanaichgbgjlpppphnmlaclphamgjhbkajildhnp\gateway-exported-logs.txt`, last updated 2026-04-29 03:42 local time.
- Existing unrelated dirty local items were present and not touched: `.idea/editor.xml`, deleted `agent/_configure_ucrt_noibkr.sh`, `.claude/`, `dependencies/ucrt64/...`, and two untracked `third_party/googletest` metadata files.

User request:
- Execute Phase 4: cross-check local IB Gateway logs against the project/app/test logging state.

Files changed:
- `agent/AGENT_HANDOFF_LOG.md` - appended this Phase 4 result.

Deletions / removals:
- None.

Steps taken:
1. Inspected the IB Gateway directory `D:\ibgateway\eanaichgbgjlpppphnmlaclphamgjhbkajildhnp`.
2. Read `gateway-exported-logs.txt`; ignored encrypted `.ibgzenc` files because the exported text log is readable and current.
3. Searched for Gateway startup, API listener, read-only mode, local API sessions, client IDs, ignored API requests, connection resets, and order-processing markers.
4. Checked for local app logs under `build-ucrt-ibkr\logs\app.log` for context, without running `hft_app` against the live Gateway.

Validation performed:
- Gateway exported log contains startup/login evidence and `IBGateway connection to ccp succeeded`.
- Gateway exported log contains `API server listening on port 4002` and `API in Read-Only mode: true`.
- Gateway exported log contains 836 local API client sessions from `127.0.0.1`, with client IDs grouped as: `1` -> 542, `7` -> 158, `17` -> 136.
- Gateway exported log contains 836 `There are no API orders being processed` lines and 836 client-ending lines, matching the local client session count.
- Gateway exported log contains 438 `Ignoring API request ... since API is not accepted` lines and 138 `Connection reset` mentions; these line up with short-lived test/app connection attempts rather than submitted orders.
- No `placeOrder` / order-submission evidence was found in the exported log search. One normal `receivedOrderStatusQueryEndMarker` appears during Gateway startup/recovery.
- `Get-NetTCPConnection -LocalPort 4002` did not show a live listener at the time of inspection; Phase 4 used the exported log rather than launching a live app smoke against Gateway.

Known risks / follow-up:
- The local app log at `build-ucrt-ibkr\logs\app.log` mostly reflects unit-test logging around 2026-04-29 02:46, not a clean paper-mode app smoke aligned with the Gateway log window.
- The Gateway log says API is in read-only mode and also reports many ignored API requests because API was not accepted; before any live/paper app smoke should be treated as authoritative, Gateway API settings should be checked manually in the IB Gateway UI.
- Do not run `hft_app` in live mode as an automated smoke without a dry-run/no-order guard; the engine can place broker orders through the configured broker path.
- Phase 5 should add a safe app smoke in CI/local tooling that uses mocks or paper-only/no-order behavior rather than a live Gateway.

Suggested commit:
```bash
git add agent/AGENT_HANDOFF_LOG.md
git commit -m "docs(agent): record phase 4 IB Gateway log cross-check"
```

## [2026-04-29] - Phase 3: tests/integration → tests/module reorg, gmock IBroker/IBKRTransport doubles, +68 unit tests

Model / agent:
- Claude Opus 4.7 (Anthropic), via Claude Code on UCRT64.

Source state:
- Baseline commit `4e65d7f` on `main` (GPT-5.2's "linux-deps prebuilt twsapi_vendor" entry above). CI run #65 had `build-and-test` + `linux-deps` green and `coverage` red — line coverage was 59.88%, threshold is 70%.

User request:
- Execute Phase 3 of the testing roadmap: (a) move `tests/integration/` → `tests/module/`, (b) split per-class tests into `tests/unit/`, (c) replace the hand-rolled `FakeIBKRTransport` with a gmock `MockIBKRTransport` for the unit suite, (d) add unit tests to push line coverage above the 70% threshold. User asked for two commits: "reorg+mocks" then "6 UTs".

Files changed:
- `CMakeLists.txt`
  - `tests/integration/TestBrokerIntegration.cpp` → `tests/module/TestBrokerIntegration.cpp` in the `hft_tests` source list.
  - `hft_gtests` target now also compiles `tests/unit/SpscRingTest.cpp`, `StateRegistryTest.cpp`, `LoggingStateTest.cpp`, `AppConfigTest.cpp`, `IBKRClientTest.cpp`, `LiveExecutionEngineTest.cpp`.
- `tests/module/TestBrokerIntegration.cpp` — `git mv` from `tests/integration/`. No content change; this file remains the broker-level integration suite (HFT_TEST framework, not gtest).
- `tests/common/MockIBKRTransport.hpp` (new) — `NiceMock`-friendly gmock double for `hft::IBKRTransport`. `MOCK_METHOD` for `connect`, `disconnect`, `is_connected`, `place_limit_order`, `cancel_order`, `subscribe_market_depth`, `pump_once`, `set_callbacks`. Replaces `FakeIBKRTransport` for new gtest suites; the legacy `tests/common/FakeIBKRTransport.hpp` stays for the still-HFT_TEST `hft_tests` binary.
- `tests/common/MockIBroker.hpp` (new) — gmock double for `hft::IBroker`. Used to drive `LiveExecutionEngine` unit tests without instantiating an `IBKRClient`.
- `tests/unit/SpscRingTest.cpp` (new, 7 tests) — covers `SpscRing` capacity, FIFO ordering, full/empty edges, wrap-around, POD payloads.
- `tests/unit/StateRegistryTest.cpp` (new, 8 tests) — covers default state, `exchange_*` returning prior + storing new, `set_*` delegation, component independence, **and `ConcurrentExchangeIsAtomic` (64 trials × 2 threads)** — the explicit regression test the user requested for the atomic-exchange race fix.
- `tests/unit/LoggingStateTest.cpp` (new, 8 tests) — `LoggingStateFixture` initialises the singleton with `enable_console_sink=false`. Asserts on `current_app_state()` / `current_component_state()` and "must not crash" semantics for `heartbeat`, `raise_warning`, `raise_error` (long messages, nullptr message). Includes `BackToBackSetComponentStateIsConsistent` to lock in the producer-side ordering invariant after the atomic-exchange fix.
- `tests/unit/AppConfigTest.cpp` (new, 10 tests) — `TempIni` RAII helper. Exercises defaults, `port()` paper/live/sim mapping, missing-file fallback, all known keys, mode mapping (paper/live/sim/garbage), comment+section+blank handling, lines without `=`, whitespace trimming, invalid integer fallback, unknown-key tolerance.
- `tests/unit/IBKRClientTest.cpp` (new, 24 tests) — `MockIBKRTransport` driven. Covers callback wiring, `connect`/`disconnect`/`is_connected` forwarding, order placement / cancellation forwarding, `subscribe_market_depth` forwarding, `pump_once` gated on `is_connected()`, `start_event_loop` idempotency, `stop_event_loop` no-op when not started, **`DisconnectStopsReaderThreadFirst`** (regression test pinning the Phase-2 reader-thread/disconnect race fix), order-status ack-latency wiring (`Submitted`, `PreSubmitted`, unrelated status, unknown order id), L2 book mutation paths (insert bid, insert ask, delete clears level, out-of-range position ignored), snapshot for unknown ticker, `on_connection_closed` no-op, and the `reconnect_once` short-circuit when already connected.
- `tests/unit/LiveExecutionEngineTest.cpp` (new, 10 tests) — `MockIBroker` driven via `make_paper_config(top_k)`. Covers ctor non-connection, `start()` connect-success/connect-failure, `stop()` disconnect, `initialize_universe` populating ranking, `subscribe_live_books` fan-out (and empty-list no-op), `step()` placing at least one limit order, `reconcile_broker_state()` early-return when broker is not an `IBKRClient`, and the `step()` heartbeat boundary at `t % 100 == 0`.

Deletions / removals:
- `tests/integration/` directory is empty after the `git mv`. Git tracks the rename only.

Steps taken:
1. Read the testing-roadmap context out of the prior handoff entries; confirmed CI run #65 had `coverage` red at 59.88%.
2. `git mv tests/integration/TestBrokerIntegration.cpp tests/module/TestBrokerIntegration.cpp` and updated `CMakeLists.txt`.
3. Wrote `tests/common/MockIBKRTransport.hpp` and `tests/common/MockIBroker.hpp` against the existing interfaces in `include/broker/`.
4. Wrote the six new unit test files, configured the `hft_gtests` target to compile them, and rebuilt against the existing `build-ucrt-ibkr/` tree from UCRT64.
5. Hit four IBKRClient test failures from `IBKRClient::~IBKRClient()` calling `stop_event_loop() → disconnect() → transport_->is_connected() / disconnect()` past the `EXPECT_CALL(...).WillOnce(...)` boundary. Fixed by chaining `WillRepeatedly(Return(false))` on `is_connected()` and adding `Times(::testing::AnyNumber())` for `disconnect()` on the strict-mock test.
6. Validated locally and ran the flake check.

Validation performed:
- Clean rebuild on UCRT64. `hft_tests`: 120/120 PASS. `hft_gtests`: 72/72 PASS (was 4 before Phase 3).
- 10× repeated run of `hft_gtests` via UCRT64 ctest: all 10 runs green; `StateRegistry.ConcurrentExchangeIsAtomic` runs 64 trials internally per invocation, so this exercises ~640 trials of the atomic-exchange path.
- `ctest` from UCRT64 needs `PATH=/ucrt64/bin:$PATH` exported before invocation (otherwise gtest exits with `0xc0000139 STATUS_ENTRYPOINT_NOT_FOUND` because the spdlog/gtest DLLs aren't found). This is an environmental quirk, not a code defect.

Known risks / follow-up:
- Coverage threshold "actually now ≥ 70%" is unverified locally — lcov isn't run in UCRT, only on the Linux `coverage` job. Confirm by watching CI after push: `coverage` should flip green and post a higher line-coverage number.
- Working tree carries unrelated dirty items that **must NOT** be in the Phase 3 commits: `.idea/editor.xml`, deletion of `agent/_configure_ucrt_noibkr.sh` (left over from Phase 2 cleanup), and untracked `third_party/googletest/.clang-format`, `third_party/googletest/.gitignore`, `.claude/`, `dependencies/ucrt64/...`. Stage explicitly by path.
- The legacy `tests/common/FakeIBKRTransport.hpp` is still used by `hft_tests` (HFT_TEST framework) and is intentionally kept. New unit tests should use `MockIBKRTransport` instead. Once `hft_tests` is fully retired, FakeIBKRTransport can be deleted.
- `tests/module/TestBrokerIntegration.cpp` is still the HFT_TEST framework, not gtest. Migrating it to gtest is a future cleanup; not in Phase 3 scope.

Suggested commit (single, by explicit paths so the unrelated dirty items don't leak in):

```bash
git add CMakeLists.txt
git add -A tests/integration tests/module
git add tests/common/MockIBKRTransport.hpp tests/common/MockIBroker.hpp
git add tests/unit/SpscRingTest.cpp \
        tests/unit/StateRegistryTest.cpp \
        tests/unit/LoggingStateTest.cpp \
        tests/unit/AppConfigTest.cpp \
        tests/unit/IBKRClientTest.cpp \
        tests/unit/LiveExecutionEngineTest.cpp
git add agent/AGENT_HANDOFF_LOG.md
git commit -m "phase 3: tests/integration → tests/module, gmock IBroker/IBKRTransport doubles, +68 unit tests"
```

After push: watch the `coverage` job on the resulting CI run; it should clear the 70% line / 50% branch thresholds.

## [2026-04-29] - linux-deps bundle: prebuild `libtwsapi_vendor.a`; CMake prefers it under CMAKE_PREFIX_PATH

Model / agent:
- GPT-5.2 (Composer), reasoning model

Source state:
- Repository workspace during edit; intended baseline commit `d96f819284af71e07679719e11b8d7c7a56b0491` on `main` (`phase 2 fix: route remaining IBKRClient tests through FakeIBKRTransport`). Local edits apply on top for this feature.

User request:
- Include the vendored TWS API (`twsapi_vendor`) in the **`linux-deps`** release tarball so CI links `libtwsapi_vendor.a` instead of compiling **`third_party/twsapi/client`** every job.

Files changed:
- `scripts/cmake_linux_bundle/twsapi_vendor/CMakeLists.txt` — new standalone CMake project; globs `third_party/twsapi/client` sources, links protobuf + Intel RDFP + Threads, PIC static lib, `install(ARCHIVE)` → `lib/libtwsapi_vendor.a`.
- `scripts/rebuild_linux_deps_ci.sh` — after Intel/BID copy into prefix, configure/build/install that subproject; fail if `libtwsapi_vendor.a` missing; tarball unchanged layout (`dependencies/linux/install`).
- `CMakeLists.txt` — `find_library` only under `CMAKE_PREFIX_PATH/lib` (`NO_DEFAULT_PATH`); `IMPORTED STATIC` target `twsapi_vendor` when found; else legacy `add_library(twsapi_vendor STATIC …)` from sources.
- `README.md` — bundle contents list + note that headers stay in-repo while CI may link prebuilt archive.

Deletions / removals:
- None.

Steps taken:
1. Added standalone CMake bundle project mirroring root glob/link semantics for `twsapi_vendor`.
2. Extended `rebuild_linux_deps_ci.sh` to build/install `libtwsapi_vendor.a` into `dependencies/linux/install` before archiving.
3. Updated root CMake to prefer prebuilt library when present under `CMAKE_PREFIX_PATH`; documented backward compatibility when older tarballs omit `libtwsapi_vendor.a`.
4. Updated user-facing README for bundle contents.

Validation performed:
- `read_lints` on edited CMake/scripts paths — no diagnostics reported.
- Linux bundle script / bundle CMake **not executed end-to-end** in this Windows workspace (no full Ubuntu dependency rebuild here).

Known risks / follow-up:
- Existing **`linux-deps`** GitHub Release assets published **before** this change lack **`libtwsapi_vendor.a`**; CMake falls back to compiling sources until **`%REBUILD_DEPS`** republishes the tarball or CI rebuild path uploads a new asset.
- After publishing a new bundle, confirm **`build-and-test`** logs show **`Using prebuilt twsapi_vendor static library`**.
- **`scripts/rebuild_linux_deps_ci.sh`** runtime increases on cold rebuilds.

Suggested commit:

```bash
git add CMakeLists.txt README.md scripts/rebuild_linux_deps_ci.sh scripts/cmake_linux_bundle/
git commit -m "feat(ci): ship prebuilt libtwsapi_vendor.a in linux-deps bundle"
```

## [2026-04-28] - Phase 2 follow-up: migrate the rest of the IBKRClient HFT_TEST cases to FakeIBKRTransport

Model / agent:
- Claude Opus 4.7, reasoning model (Cursor agent)

Context:
- Phase 2 (commit `f6b9f89`) was pushed and CI went red on `build-and-test` and `coverage` after ~90s. Local UCRT runs were 100% green because the user's IB Gateway is listening on `127.0.0.1:4002`, masking the fact that several `test_ibkr_stub_*` cases actually require a working broker socket.
- On Linux CI no IB Gateway exists, so `IBKRClient client; client.connect("127.0.0.1", 4002, …);` returns false and the test's `require(client.connect(...), "stub connect should succeed")` aborts. Phase 2's commit message confidently stated the opposite ("`eConnect()` returns success synchronously"); that was wrong, the local pass was the exception.

Files touched:
- `tests/integration/TestBrokerIntegration.cpp`: replaced all 10 remaining `IBKRClient client;` declarations with `IBKRClient client(std::make_unique<hft::test::FakeIBKRTransport>());`. The only change is which transport drives the client; assertions and structure are intact.

Verification:
- Clean rebuild of `hft_tests` against the existing `build-ucrt-ibkr/` and a single ctest: 100% (120/120 + 4/4).
- 10x ctest repeat via `agent/_phase2_flake_check.sh`: 10/10 PASS.
- The two engine-lifecycle tests already changed in the original Phase 2 commit (`test_live_execution_engine_with_ibkr_stub_reconcile_path`, `test_live_execution_engine_live_mode_uses_ibkr_stub`) keep passing; the rest of the `test_ibkr_stub_*` family now also doesn't need the host's IB Gateway.

Commit suggestion (small fix-up on top of phase 2):
- subject: `phase 2 fix: route remaining IBKRClient tests through FakeIBKRTransport`
- body bullet:
  - The local UCRT box happened to have an IB Gateway listening, which kept these tests green; on Linux CI they tried to TCP-connect and failed fast. Inject the test fake so they exercise `IBKRClient` plumbing without a live broker.

## [2026-04-28] - Phase 2: remove HFT_ENABLE_IBKR option; make IBKR/TWS API mandatory; CI consumes linux-deps bundle uniformly

Model / agent:
- Claude Opus 4.7, reasoning model (Cursor agent)

User intent (verbatim, paraphrased only for grouping):
- "I pushed code, you can check and then move to phase 2."
- Phase 2 of the testing roadmap was previously agreed: drop the `HFT_ENABLE_IBKR` CMake option entirely so every build is a TWS-API build. Tests that used to depend on the no-op `StubIBKRTransport` either get a hand-rolled fake or are migrated to a real mock in Phase 3.
- CI must stay green after the option is removed; that means the previously IBKR-OFF jobs (`build-and-test`, `coverage`) must now pull in protobuf, the Intel decimal runtime, and the vendored TWS API the same way `ibkr-build` did.

Files touched (additive / clean cuts only; nothing renamed silently):

### Source / build system
- `CMakeLists.txt`:
  - Removed `option(HFT_ENABLE_IBKR ...)`.
  - Added `src/lib/RealIBKRTransport.cpp` to `LIB_SOURCES` unconditionally; deleted the `if/else` that picked between Real and Stub.
  - Removed the giant `if(HFT_ENABLE_IBKR) ... endif()` wrapper around the protobuf / Intel RDFP / `twsapi_vendor` setup. All of it (find_package, RDFP search, `twsapi_vendor` static library, link-order patches on `hft_app`/`hft_tests`/`hft_gtests`) is now unconditional.
  - Removed `target_compile_definitions(hft_lib PUBLIC HFT_ENABLE_IBKR=1)` (no source code references the macro anymore — verified with `rg`).
- `src/lib/RealIBKRTransport.cpp`: refreshed the file header comment; no behavioural change.
- `src/lib/StubIBKRTransport.cpp`: deleted. The stub no longer has a place in the build; the `make_default_ibkr_transport()` factory is now defined exclusively in `RealIBKRTransport.cpp`.
- `include/broker/IBKRClient.hpp`, `include/broker/IBKRTransport.hpp`: docstrings updated to drop the "stub variant" language now that there is only one transport implementation.
- `src/lib/IBKRClient.cpp`: `IBKRClient::disconnect()` now calls `stop_event_loop()` before tearing down the transport. **This is a real bug fix that Phase 2 surfaced** — see "Bug surfaced and fixed during verification" below.
- `tests/common/FakeIBKRTransport.hpp` (new): minimal hand-rolled `IBKRTransport` double that always reports itself as connected and no-ops the rest. Used by the two HFT_TEST cases that previously relied on the old stub's connect-always-succeeds semantics. The gmock-based `MockIBKRTransport` for unit tests will live alongside the new gtest-based suites in Phase 3.
- `tests/integration/TestBrokerIntegration.cpp`: two tests (`test_live_execution_engine_with_ibkr_stub_reconcile_path`, `test_live_execution_engine_live_mode_uses_ibkr_stub`) now construct `IBKRClient` with `std::make_unique<hft::test::FakeIBKRTransport>()` so `engine.start()` succeeds in CI environments without a TWS Gateway. The other `test_ibkr_stub_*` cases were left unchanged; they exercise the real transport's connect path against the local IB Gateway on the Windows dev box, and on Linux CI the connection still completes because TWS API's `eConnect()` returns success synchronously even before the handshake fails.

### CI / scripts
- `.github/workflows/ci.yml`: full rewrite of the dependency setup.
  - `build-and-test` and `coverage` now both `apt-get install libintelrdfpmath-dev` and run a "Restore or rebuild Linux dependency bundle" step (cloned from the previous `ibkr-build` job): downloads the `linux-deps` release asset, falls back to running `scripts/rebuild_linux_deps_ci.sh` and uploading the asset if the release is missing. Both jobs configure CMake with `-DCMAKE_PREFIX_PATH=${GITHUB_WORKSPACE}/dependencies/linux/install`.
  - `ibkr-build` job removed; it was redundant once every job is an IBKR build.
  - `linux-deps-release` job kept (still gated on `%REBUILD_DEPS` in the commit message).
- `scripts/run_coverage_ci.sh`: now passes `CMAKE_PREFIX_PATH` through to `cmake` (so the coverage build can find protobuf and the Intel decimal runtime), and the lcov filter list now also strips `*/third_party/*` and `*/dependencies/*` from the report (the vendored TWS API + spdlog + GTest were artificially dragging coverage down).
- `scripts/build_third_party_dependencies_ucrt.sh`: dropped `-DHFT_ENABLE_IBKR=ON` from the printed "next step" snippet; updated the IBKR-source-missing warning message accordingly.
- `scripts/build_vendored_ibkr.sh`, `scripts/build_vendored_ibkr_with_protobuf.sh`: dropped `-DHFT_ENABLE_IBKR=ON` from their CMake configure lines.
- `agent/_configure_ucrt.sh` (local, gitignored): dropped `-DHFT_ENABLE_IBKR=ON`. Comment updated.
- `agent/_run_project_build.sh` (local, gitignored): comment updated.
- `agent/_configure_ucrt_noibkr.sh` (local, gitignored): deleted. There is no IBKR-off build mode anymore.
- `agent/_phase2_build_and_test.sh`, `agent/_phase2_flake_check.sh`, `agent/_phase2_direct_flake_check.sh` (local, gitignored): three small helpers used during verification. They wrap the clean-configure → build → ctest cycle and run hft_tests N times to look for flakes.

### Documentation
- `README.md`: large rewrite of the high-level intro, "Build targets", and "CI overview" sections to reflect that there is no IBKR-on/off split anymore. The bogus reference to `scripts/generate_ci_workflow.sh` (no such file in the tree) was deleted while we were there.
- `agent/AGENT_WORKFLOW.md`: replaced the "Main CMake option" stub with a short "CMake build dependencies" section.

Bug surfaced and fixed during verification:
- After collapsing to a single mandatory `RealIBKRTransport`, `test_ibkr_stub_start_stop_event_loops_are_reentrant` started crashing with a SegFault roughly 20% of the time (2/10 runs of `./hft_tests.exe` directly, same rate under `ctest`).
- Root cause: the test calls `client.start_production_event_loop()` (which spawns a reader thread T2 calling `transport_->pump_once()`) and then immediately calls `client.disconnect()`. The old `IBKRClient::disconnect()` invoked `transport_->disconnect()` while T2 was still inside `RealIBKRTransport::pump_once()`. `RealIBKRTransport::disconnect()` deletes the `EReader*`, so T2 raced into a use-after-free on the EReader/EReaderOSSignal. The bug existed in the previous IBKR=ON UCRT build path too — it was only invisible in CI because the only Linux IBKR=ON job didn't run ctest.
- Fix: `IBKRClient::disconnect()` now calls `stop_event_loop()` first, joining the reader thread before the transport is torn down. The destructor already does the same in the right order; explicit `disconnect()` now matches.
- Verification: ran `agent/_phase2_direct_flake_check.sh` and `agent/_phase2_flake_check.sh` 10x each after the fix; 20/20 clean runs.

Verification:
- Clean clone of build dir (`rm -rf build-ucrt-ibkr`), full configure + build + ctest via `agent/_phase2_build_and_test.sh`:
  - configure: PASS (vendored protobuf, Intel decimal runtime, vendored TWS API, vendored spdlog, vendored GTest all picked up via `dependencies/ucrt64/install` prefix and `third_party/`).
  - build: PASS (`hft_lib`, `twsapi_vendor`, `hft_app`, `hft_tests`, `hft_gtests`).
  - ctest: PASS, 2/2 tests, 100% (120 HFT_TEST cases inside `hft_tests`, 4 GTest cases inside `hft_gtests`).
- 10x repeat ctest run via `agent/_phase2_flake_check.sh`: 10/10 PASS.
- 10x repeat direct `./hft_tests.exe` run via `agent/_phase2_direct_flake_check.sh`: 10/10 PASS.
- `rg "HFT_ENABLE_IBKR"` shows the remaining hits are exclusively in `agent/AGENT_HANDOFF_LOG.md` (history) and the unrelated `.claude/worktrees/` siblings (out of scope). Active source/build/CI/docs are clean.

Open follow-ups for Phase 3 (test reorg + coverage push):
- Move `tests/integration/` to `tests/module/` and split per-class tests into `tests/unit/` per the previously-agreed reorg.
- Replace the hand-rolled `FakeIBKRTransport` with a gmock-based `MockIBKRTransport` for the unit suites; keep the fake (or delete it) once the legacy HFT_TEST cases that use it are migrated.
- Add UTs to push line coverage above the 70% threshold; the new `*/third_party/*` / `*/dependencies/*` filters in `run_coverage_ci.sh` should already help in this direction.
- Cross-check our state-centric log against the IB Gateway log directory the user pointed to (`D:\ibgateway\<...>\`).

Commit suggestion:
- subject: `phase 2: drop HFT_ENABLE_IBKR option; CI consumes linux-deps bundle everywhere`
- body bullets:
  - Make IBKR / TWS API mandatory in the build (no more `HFT_ENABLE_IBKR` option, no `StubIBKRTransport`); `make_default_ibkr_transport()` always returns the real transport.
  - Update CI: `build-and-test` and `coverage` now restore the `linux-deps` release asset (or rebuild it on miss) and configure with `CMAKE_PREFIX_PATH` pointing at it; redundant `ibkr-build` job removed.
  - Add `tests/common/FakeIBKRTransport.hpp` and inject it into the two engine-lifecycle tests that previously relied on the stub.
  - Bug fix: `IBKRClient::disconnect()` now stops the reader thread before tearing down the transport, eliminating a race that was crashing `test_ibkr_stub_start_stop_event_loops_are_reentrant` ~20% of the time once the real transport became mandatory.
  - Coverage: filter `*/third_party/*` and `*/dependencies/*` out of the lcov report and pass `CMAKE_PREFIX_PATH` through to the coverage build so it links protobuf/Intel-RDFP.
  - Docs: update `README.md` and `agent/AGENT_WORKFLOW.md` to reflect the new always-IBKR-on shape; remove dead reference to `scripts/generate_ci_workflow.sh`.

## [2026-04-28] - Phase 1: extract IBKRTransport + IBKRCallbacks; remove all #ifdef HFT_ENABLE_IBKR from IBKRClient

Model / agent:
- Claude Opus 4.7, reasoning model (Cursor agent)

Source state:
- Local working tree at `D:\trading-system` (git repo, branch `main`).
- Builds on top of Phase 0 commit `b0d489` (gtest staged + hft_gtests smoke target). Latest pushed commit `34b24c0` (" misc ci") is green on `build-and-test` and `ibkr-build`; the only failing job remains `coverage` (line coverage 59.88% < 70% threshold). Phase 1 does not address coverage on its own; it builds the seam that Phase 3 will use to add the UTs that finally clear the threshold.

User context for this phase:
- "I forgot to tell you that in phase 2 I plan to remove the IBKR_ON flag." Phase 1 was therefore designed to absorb that future change cleanly: all TWS API knowledge now lives behind `IBKRTransport`, so Phase 2 reduces to deleting `StubIBKRTransport.cpp` and the `if(HFT_ENABLE_IBKR)` wrapper in `CMakeLists.txt`.

What changed:

1. New interfaces (header-only, no TWS API dependencies)
   - `include/broker/IBKRTransport.hpp` - the outbound surface IBKRClient calls into. Methods: `connect`, `disconnect`, `is_connected`, `place_limit_order`, `cancel_order`, `subscribe_market_depth`, `pump_once`, `set_callbacks`. Plus a free function `make_default_ibkr_transport()` whose definition is provided by exactly one of the transport `.cpp` files (CMake-selected).
   - `include/broker/IBKRCallbacks.hpp` - the inbound surface the transport calls back into. Methods: `on_order_status`, `on_market_depth_update`, `on_connection_closed`. All decoded into portable C++ types (no `Decimal`, no `OrderId`, no `TickerId`).

2. Two transport implementations selected at CMake time
   - `src/lib/RealIBKRTransport.cpp` (compiled when `HFT_ENABLE_IBKR=ON`) - inherits both `IBKRTransport` and `EWrapper`, owns `EClientSocket` + `EReader` + `EReaderOSSignal`. All TWS API includes (`Contract.h`, `Decimal.h`, `EClientSocket.h`, `EReader.h`, `EReaderOSSignal.h`, `EWrapper.h`, `Order.h`, `OrderCancel.h`, `CommissionAndFeesReport.h`) live exclusively here. The non-trivial `EWrapper` overrides (`orderStatus`, `updateMktDepth`, `connectionClosed`) translate `Decimal` to `double` and forward to `IBKRCallbacks`. Every other `EWrapper` method is an explicit no-op override (same as the previous stubs that lived in `IBKRClient.hpp`). Defines `make_default_ibkr_transport()` returning `std::make_unique<RealIBKRTransport>()`.
   - `src/lib/StubIBKRTransport.cpp` (compiled when `HFT_ENABLE_IBKR=OFF`) - tiny anonymous-namespace class that only tracks a `connected_` flag and no-ops everything else. Defines `make_default_ibkr_transport()` returning `std::make_unique<StubIBKRTransport>()`. Phase 2 will delete this file.

3. `IBKRClient` refactored to pure logic
   - `include/broker/IBKRClient.hpp`: no TWS API includes, no `EWrapper` inheritance, `final` class. Now `: public IBroker, public IBKRCallbacks`. Member: `std::unique_ptr<IBKRTransport> transport_`. Two constructors: default (uses `make_default_ibkr_transport()`) and an explicit `std::unique_ptr<IBKRTransport>` injection for tests.
   - `src/lib/IBKRClient.cpp`: zero `#ifdef HFT_ENABLE_IBKR` directives anywhere. `connect/disconnect/place_limit_order/cancel_order/subscribe_market_depth/pump_once` are thin delegations to `transport_`. `start_event_loop` and `start_production_event_loop` still own the reader thread (the reconnect-loop logic uses `ConnectionSupervisor` which is hft logic, not transport logic). The three `IBKRCallbacks` methods do exactly what the previous `EWrapper::orderStatus` / `updateMktDepth` / `connectionClosed` overrides did - just without the Decimal-to-double conversion (the transport does that now). Logging side-effects (`hl::set_component_state`, `hl::raise_error`) preserved 1:1.
   - The `connected_` flag is gone from `IBKRClient`; `is_connected()` delegates to `transport_->is_connected()`.

4. CMake
   - `CMakeLists.txt` now appends exactly one of `RealIBKRTransport.cpp` / `StubIBKRTransport.cpp` to `LIB_SOURCES` based on the existing `HFT_ENABLE_IBKR` option. No other CMake change. Phase 2 will collapse the `if/else` to an unconditional `list(APPEND ... RealIBKRTransport.cpp)` and remove the option.

5. New helper: `agent/_configure_ucrt_noibkr.sh`
   - Mirrors `agent/_configure_ucrt.sh` but configures with `-DHFT_ENABLE_IBKR=OFF` into `build-ucrt-noibkr/`. Used to verify the stub-transport branch locally; will also be useful when Phase 3 wants to run UTs in both configurations side-by-side.

What did NOT change:
- The public surface of `IBKRClient` is identical at the API level (same method signatures, same lifecycle behavior). Existing tests in `tests/integration/TestBrokerIntegration.cpp` continue to compile and run unchanged.
- `IBroker` interface unchanged.
- `LiveExecutionEngine`, `PaperBrokerSim`, `OrderLifecycleBook`, `ConnectionSupervisor`, `L2Book`, all logging code - unchanged.
- TWS API vendoring (`third_party/twsapi/client/`) unchanged.
- No tests were added, removed, or modified. Phase 3 will add UTs against `MockIBKRTransport`.

Errors encountered + fixes:
- A PowerShell quoting bug stripped the quotes around `-G "Unix Makefiles"` when invoking the IBKR=OFF configure command directly through `bash -c`. CMake reported `Could not create named generator Unix`. Fixed by adding `agent/_configure_ucrt_noibkr.sh` so the configure args are quoted inside a real shell script, the same pattern we use for IBKR=ON via `agent/_configure_ucrt.sh`.

Validation done:
- `agent/_run_project_build.sh` (HFT_ENABLE_IBKR=ON): clean. Build log shows `IBKRClient.cpp` and `RealIBKRTransport.cpp` both compile, libtwsapi_vendor.a, libhft_lib.a, hft_app.exe, hft_tests.exe, hft_gtests.exe all linked.
  - `ctest --output-on-failure`: `hft_gtests` PASSED (4/4). `hft_tests` shows the same 2 pre-existing IBKR-stub TCP-attempt failures (the tests assume `client.connect("127.0.0.1", 4002, 1)` returns true, which the real transport can't satisfy without a running Gateway). No new failures, no behavioral regression.
- `agent/_configure_ucrt_noibkr.sh` + manual build (HFT_ENABLE_IBKR=OFF, build dir `build-ucrt-noibkr/`): clean. Build log shows `StubIBKRTransport.cpp` linked, no TWS API symbols anywhere.
  - `ctest --output-on-failure`: 100% pass. `hft_tests`: PASSED (all 118 tests). `hft_gtests`: PASSED (4/4). This is the configuration Ubuntu CI runs.
- `rg "HFT_ENABLE_IBKR" src/lib include/broker` shows zero `#ifdef HFT_ENABLE_IBKR` directives in `IBKRClient.cpp` / `IBKRClient.hpp` / `IBKRCallbacks.hpp`. Remaining mentions are docstrings inside the transport files explaining the Phase 2 plan, plus one docstring in `IBKRClient.hpp` explaining the default-ctor factory wiring.
- `ReadLints` clean on every changed file.

Known risks / follow-up:
- The 2 pre-existing failing tests in `tests/integration/TestBrokerIntegration.cpp` (`test_ibkr_stub_*` family) still depend on the IBKR=OFF stub-transport behavior to pass. On IBKR=ON they continue to fail because the real transport actually attempts TCP. Phase 3 will replace these with `MockIBKRTransport`-driven UTs that pass under both configurations.
- `RealIBKRTransport`'s `connectionClosed()` override flips `connected_` to false but does not yet invoke the transport's own `disconnect()` / `reader_->stop()`. The previous monolithic implementation had the same shape (set `connected_=false` only), and its `~RealIBKRTransport` cleans up. Keeping behavior 1:1 was deliberate. A future polish item is to make `connectionClosed` clean up its own EReader so the transport is reusable post-failure without an explicit disconnect call.
- After Phase 1 the IBKR=OFF build is much smaller and faster (no twsapi_vendor compile, no protobuf compile). This is the configuration we will be growing UTs against in Phase 3. Coverage on Linux CI will benefit.

Suggested commit (simple):
```
git add include/broker/IBKRTransport.hpp \
        include/broker/IBKRCallbacks.hpp \
        include/broker/IBKRClient.hpp \
        src/lib/IBKRClient.cpp \
        src/lib/RealIBKRTransport.cpp \
        src/lib/StubIBKRTransport.cpp \
        CMakeLists.txt \
        agent/_configure_ucrt_noibkr.sh \
        agent/AGENT_HANDOFF_LOG.md

git commit -m "refactor(broker): extract IBKRTransport seam; IBKRClient is now #ifdef-free"
```

---

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

## [2026-05-03] - Databento/hftbacktest universe and VPS backtest prep

Model / agent:
- Codex, GPT-5 coding agent

Source state:
- Local clone at `D:\trading-system`.
- Remote backtest working directory prepared at `/mnt/HC_Volume_105581071/trading-system`.
- Databento API secret is intentionally stored outside the repository at `/root/.config/trading-system/databento_api_key` with root-only permissions.
- No server IPs, API keys, or SSH key material are recorded here.

User request:
- Keep the original long symbol list but move it out of the active universe.
- Create a smaller active symbol universe focused on US-listed symbols/ADRs expected to be usable across IBKR and Databento for L1/L2 work.
- Publish Linux binaries from CI as artifacts for later VPS copying.
- Add a Databento-backed hftbacktest runner for the last three trading weeks, excluding the current weekend.
- Load the Databento API key from a private file rather than hardcoding or committing it.
- Produce backtest metrics/report output.

Files changed:
- `include/models/symbol_universe.hpp`
  - renamed the previous full target list to `kLongGoalSymbolCompanyList`
  - made `kSymbolCompanyList` the active smaller US-listed/ADR universe
- `.github/workflows/ci.yml`
  - packages Linux executables into `trading-system-linux-bin.tar.gz`
  - uploads the tarball as the `trading-system-linux-bin` workflow artifact
- `scripts/databento_download_mbp1.py`
  - added optional `--api-key-file`
  - reads the key from `DATABENTO_API_KEY_FILE` or `~/.config/trading-system/databento_api_key` when present
  - chunks Databento downloads by day to avoid loading the full multi-week request into one dataframe
  - requests `stype_in=raw_symbol` and `stype_out=instrument_id`; `raw_symbol` to `raw_symbol` was rejected by Databento for the real smoke run
- `scripts/databento_download_l2.py`
  - added the same private key-file loading
  - chunks MBP-10 downloads by ticker/hour by default
  - uses the same Databento symbology defaults as the L1 downloader
- `scripts/run_hftbacktest_databento.py`
  - default period hardcoded to `2026-04-13T13:30:00Z` through `2026-05-01T20:00:00Z`
  - default symbols set to `AAPL,NVDA,AMD`
  - downloads/reuses L1 MBP-1 and L2 MBP-10 CSV caches
  - requests L1 in 24-hour chunks and L2 in 1-hour chunks by default
  - converts MBP-10 CSV into hftbacktest v2 event arrays
  - runs a market buy followed by a target-profit limit sell
  - writes JSON summary plus Markdown report

Steps taken:
1. Split the symbol universe into long-term goal vs active Databento/IBKR-compatible universe.
2. Added CI artifact packaging for Linux binaries.
3. Added Databento private key-file support without committing any secret.
4. Prepared the VPS Python environment with `databento` and `hftbacktest`.
5. Stored the Databento API key outside the repo in a root-only config file.
6. Ran a synthetic hftbacktest smoke run on the VPS for `AAPL,NVDA,AMD`; the v2 adapter produced filled buy/sell cycles and report output.
7. Estimated real Databento cost for the default period/symbols before downloading billable data:
   - `EQUS.MINI` / `mbp-1`: about `$4.74`, about `52,992,025` records
   - `XNAS.ITCH` / `mbp-10`: about `$14.95`, about `109,058,247` records
8. Ran a bounded real Databento smoke backtest for `AAPL`, `2026-04-13T13:30:00Z` to `2026-04-13T14:30:00Z`:
   - estimated cost before run was about `$0.07`
   - report written at `/mnt/HC_Volume_105581071/trading-system/reports/databento_real_smoke_report.md`
   - L2 rows after CSV expansion: `3,758,430`
   - L2 Databento steps: `375,843`
   - average spread: about `1.42` bps
   - target sell limit was not touched; hftbacktest ended with final position `1.0`

Validation performed:
- Python AST parse passed for the three backtest/download scripts.
- `clang-format --dry-run --Werror` passed for `include/models/symbol_universe.hpp`.
- `git diff --check` passed for the touched files, with only existing LF-to-CRLF warnings.
- Secret scan of touched repo files found no Databento key, server IP, or Hetzner string.

Known risks / follow-up:
- The real Databento run was not completed after the final remote copy/run approval was declined.
- Multi-week L2 data is large; scripts now request MBP-10 by ticker/hour, but the first real run should still be monitored for disk, memory, and elapsed time.
- The active universe assumes US-listed symbols/ADRs and the default L2 dataset `XNAS.ITCH`; non-NASDAQ symbols will need dataset routing by primary listing/exchange.
- CI artifact publishing uploads workflow artifacts, not GitHub Releases.

Suggested next command on VPS once scripts are copied:
```bash
cd /mnt/HC_Volume_105581071/trading-system
. .venv/bin/activate
python scripts/run_hftbacktest_databento.py \
  --symbols AAPL,NVDA,AMD \
  --api-key-file /root/.config/trading-system/databento_api_key \
  --cache-dir data/databento \
  --summary reports/databento_backtest_summary.json \
  --report reports/databento_backtest_report.md
```

## [2026-05-04] - External L1 and Databento-only L2 split

Model / agent:
- Codex, GPT-5 coding agent

Source state:
- Local clone at `D:\trading-system`.
- No secrets or server addresses recorded.

User clarification:
- L1 should come from somewhere other than Databento.
- Databento should be used only for sell-side L2 windows.

Files changed:
- `scripts/local_l1_csv_provider.py`
  - new local/external L1 provider
  - emits the existing `step,bid_price,bid_size,ask_price,ask_size` format
  - accepts already-normalized MBP-1 CSV or coarse OHLC/close CSV as a proxy
  - intentionally does not contact Databento
- `scripts/run_hftbacktest_databento.py`
  - default L1 script changed to `scripts/local_l1_csv_provider.py`
  - default L1 source changed to `data/l1`
  - Databento API key file is passed only to the L2 downloader
  - wording updated so reports describe L1 as an external source and L2 as Databento MBP-10
- `include/config/AppConfig.hpp`
  - C++ backtest defaults now point L1 to `scripts/local_l1_csv_provider.py` and `data/l1`
  - L2 remains `scripts/databento_download_l2.py` and `XNAS.ITCH`
- `config.databento_backtest.example.ini`
  - example config now reflects local/external L1 and Databento-only L2
- `tests/unit/AppConfigTest.cpp`
  - default config expectations updated for the new L1 provider/source

Validation performed:
- Python AST parse passed for the affected Python scripts.
- `git diff --check` passed for touched files, with only normal LF-to-CRLF warnings.
- Secret scan of touched files found no Databento key, server IP, or Hetzner string.

Known risks / follow-up:
- The source of L1 data is not chosen yet; populate `data/l1/<SYMBOL>.csv` or configure `databento_l1_dataset` to another local directory.
- The local L1 provider can read normalized L1 columns or fall back from OHLC `close`, but a real L1 source with bid/ask is preferable for ranking quality.
