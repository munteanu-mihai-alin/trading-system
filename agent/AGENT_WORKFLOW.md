# Agent Workflow

This file defines the shared workflow for agents working on this repository. It should stay mostly stable. Active per-interaction notes belong in `AGENT_HANDOFF_LOG.md`.

## Purpose

The goal is to let multiple models or agents work on the same project without losing context or repeating mistakes. Agents should use this workflow to understand how to inspect the project, make changes, validate them, and hand off useful state to the next agent.

## Backtest run reporting

Every backtest run produces a per-run folder at
`reports/runs/<YYYY-MM-DDTHHMM>_<label>/` with this layout:

```text
manifest.json   - structured metadata (started_at, ended_at, harness,
                  outcome, end-of-run stat lines, artifact list, notes)
stdout.log      - full engine output (when available)
decisions.csv   - per-buy snapshot of all ranked symbols (C++ backtest)
report.md       - markdown summary (Python harness output)
summary.json    - JSON summary (Python harness output)
config.ini      - exact config used (C++ backtest)
```

After a backtest finishes, run:

```bash
python3 scripts/organize_runs.py
```

It groups loose files under `reports/` (e.g. `cpp_backtest_stdout.log`,
`decisions.csv`, `oneday_aapl_*`, `databento_*`) into per-run folders,
infers timestamps from spdlog brackets (or file mtime), writes a
`manifest.json` with the end-of-run summary parsed out of stdout, and
regenerates `reports/runs/index.md` as a Markdown table of every known
run. The script is idempotent - safe to re-run; it skips folders that
already have a `manifest.json`.

### Per-row `ts_ns` and session markers

Every row in `decisions.csv`, `orders.csv`, `step_trace.csv`, and
`l2_trace.csv` starts with a `ts_ns` column - wall-clock nanoseconds
since the Unix epoch (`std::chrono::system_clock`). This lets you query
"events on day X" or "orders between 14:00 and 15:00" without inferring
time from the engine `step` index.

Each log file is delimited by **session markers** that the engine
writes as `#`-prefixed comment lines (pandas `read_csv(comment='#')`
skips them):

```
# session_start ts=2026-05-18T02:21:09.981Z ts_ns=... kind=order mode=databento_backtest label=cpp_backtest universe_size=50
ts_ns,step,order_id,symbol,...
... data rows ...
# session_end ts=2026-05-18T02:37:28.165Z ts_ns=... kind=order orders_placed=4 open_positions=1
```

`AppConfig::log_append_mode` controls how restarts are handled:

- `false` (default, recommended for backtest): each engine start
  truncates the file. Since backtests already isolate sessions via
  `reports/runs/<run_id>/`, fresh files inside each run folder is the
  right choice.
- `true` (recommended for live): each engine start appends to the
  existing file. Restarts within the same day produce one continuous
  file with two `session_start` lines; the gap between the prior
  `session_end` and the next `session_start` is the system's downtime.

### Manifest schema

```json
{
  "run_id": "<YYYY-MM-DDTHHMM>_<label>",
  "harness": "cpp_hft_app" | "python_run_hftbacktest_databento",
  "label": "<short description>",
  "started_at": "<ISO-8601 with Z>",
  "ended_at":   "<ISO-8601 with Z>",
  "outcome":    "completed" | "killed" | "crashed" | "unknown",
  "orders_placed": 0,
  "open_positions": 0,
  "open_notional": 0.0,
  "latency_line":     "Latency (cycles): ...",
  "validation_line":  "Validation: ...",
  "artifacts": ["stdout.log", "decisions.csv", "config.ini", "manifest.json"],
  "notes":     "<free text>"
}
```

When a run is on the Hetzner VPS, also `scp -r hetzner:/mnt/.../reports/
runs/. reports/runs/` so the local mirror stays in sync. The heavy L2
caches stay under `data/l2/<window>/` and are deliberately NOT copied
into per-run folders.

## Backtest launch workflow

Prefer the shell scripts at `scripts/hft_*.sh` over manually composed
`ssh hetzner '...'` strings. They encode the steps below in one place,
are idempotent, and save a lot of round-tripping. All three are intended
to be run from the local UCRT64 shell (they handle the `ssh`/`scp`
themselves).

### End-to-end backtest

```bash
# Rerun the last backtest window with a different target:
scripts/hft_backtest.sh --target 0.012 --label v8_target012

# Run the COVID adversarial scenario (uses its own config file):
scripts/hft_backtest.sh --config config.databento_backtest.covid.ini

# Launch only; come back to it later via scripts/hft_postmortem.sh:
scripts/hft_backtest.sh --target 0.005 --label v_partial --no-archive --no-sync --no-plot
```

`scripts/hft_backtest.sh` does, in order: SCP the config (if `--config`
points to a local file) -> apply `sed` overrides in-place on the remote
config -> clear stale flat `reports/*.csv` -> `nohup` launch -> wait for
the engine to exit (poll every 30 s) -> archive the flat outputs into
`reports/runs/<YYYY-MM-DDTHHMM>_<label>/` on Hetzner -> SCP that folder
back locally -> run `scripts/plot_run.py` against it -> print the
generated `metrics.md`.

Useful flags: `--start <iso>` / `--end <iso>` override the window;
`--symbols <file>` overrides `symbol_universe_path`; `--kill-first`
terminates any in-flight `hft_app` before launching; `--no-wait`,
`--no-archive`, `--no-sync`, `--no-plot` make the script stop earlier;
`--dry-run` echoes commands without running them.

### L1 backfill — choosing IBKR vs Databento source

```bash
# Default = IBKR (free, ~4h IBKR-pacing-bound for full universe):
scripts/hft_l1_backfill.sh yen

# Databento MBP-1 (recommended for any window >3 months old):
scripts/hft_l1_backfill.sh yen --source databento

# Backfill COVID window via Databento (the only sensible choice -
# IBKR's split-adjustment makes 6-year-old data unusable):
scripts/hft_l1_backfill.sh covid --source databento
```

Three window presets baked in (`yen`, `covid`, `10day`). All write into
the dated layout `data/l1/<startDate>_<endDate>/<SYMBOL>_<startISO>_<endISO>.mbp1.csv`
(see `include/broker/cache_filename.hpp`).

| Aspect | `--source ibkr` (default) | `--source databento` |
|---|---|---|
| Where it runs | Locally (needs IB Gateway on port 4002) | On Hetzner via ssh (needs API key + databento python pkg, both already there) |
| Cost | Free | ~$0.15-0.30/symbol per ~10-day window (~$7-12 for full 49-symbol universe per window) |
| Wall time | ~5 min/symbol serial (IBKR rate-limits) -> ~4h for 49 sym | Parallel-friendly; ~10-30 min for 49 sym via xargs -P 16 |
| Price adjustment | **Split-adjusted retroactively** by IBKR (no opt-out) | Raw exchange prints, never adjusted |
| Right for | Very recent windows (<3 months from "now") where no splits have happened since | Adversarial / historical windows (Yen 2024-08, COVID 2020-03) |

**Rule of thumb**: prefer `--source databento` for any window where stocks could have split since then. The IBKR free path is fine for windows in the recent past, but for COVID (6 years out) or Yen (22 months out) the L1 vs L2 disagreement is severe (30-50% on split-affected symbols) and silently corrupts ranking + sizing decisions. See the 2026-05-28 handoff entry for the full root-cause writeup.

Cost-quote helper before committing spend:

```bash
ssh hetzner 'cd /mnt/HC_Volume_105581071/trading-system && \
  .venv/bin/python scripts/databento_l1_cost_quote.py --also-mbp10'
```

Calls Databento's free `metadata.get_cost()` and prints MBP-1 + MBP-10 quotes for all three preset windows.

### Comparing runs side-by-side

```bash
scripts/compare_runs.py                          # latest 8 runs as markdown
scripts/compare_runs.py <id1> <id2> ...          # explicit list
scripts/compare_runs.py --label-contains target  # filter by substring
```

Reads `metrics.json` from each `reports/runs/<run_id>/` and prints a markdown table (also saved to `reports/runs/_compare.md`). Columns: trip count, realized PnL, win rate, avg per-trade $, open count, avg holding minutes, annualised Sharpe, net PnL after opportunity cost, unrealized PnL, deepest drawdown %. Newer metrics (honest-loss) show "-" for older runs whose metrics.json predates them.

### Situational awareness

```bash
scripts/hft_status.sh           # one-shot snapshot of Hetzner + local
scripts/hft_status.sh --kill    # also terminate any running hft_app +
                                # orphan Python downloaders on Hetzner
scripts/hft_status.sh --tail    # print the full latest log tail
```

Shows the current `hft_app` PIDs, latest log tail (HEALTH lines
filtered), recent order placements, newest L2 cache entries, free RAM,
and the count of files in each local `data/l1/<window>/`. Useful between
turns to confirm whether a run is still in progress without paying ssh
latency for each individual field.

### When to fall back to manual ssh

The scripts cover the 95% case. Reach for hand-composed `ssh` when:
- you need to inspect a specific raw L1/L2 cache file's contents,
- you're triaging a partial run mid-flight and need bespoke greps over
  the running log,
- the binary has changed in a way the scripts don't yet account for
  (e.g. new mandatory config knob).

If the same hand-composed command shows up twice in a session, fold it
into one of the scripts above and document it here.

## Buy / sell engine flow

Step-by-step trace of what `LiveExecutionEngine::step(t)` does on each
tick. Useful when reading orders.csv after a run -- this is what
produced each row. Source of truth is `src/lib/LiveExecutionEngine.cpp`;
this section summarises it.

### Buy (entry) side

Order of operations inside `step(t)`:

1. **`broker_->on_step(t)`** -- advances the backtest replay clock (no-op
   for live brokers).
2. **`reconcile_broker_state()`** -- per item, calls
   `snapshot_top_of_book(ticker_id=i+1)` and updates the Stock's
   `mid`, `bid_price`, `ask_price`, `queue` from L1. Drains trade
   prints into the Hawkes mid-change proxy when
   `hawkes_use_real_trades` is set.
3. **`refresh_order_state()`** -- walks open entry + exit orders against
   `OrderLifecycleBook`; updates `open_positions_` on terminal statuses
   (Filled / Cancelled / Rejected).
4. **`check_daily_loss_kill_alert()`** -- alert-only, never destructive.
5. **`check_user_kill_switch()`** -- polls `kill_signals::user_kill_requested()`
   (SIGUSR1 atomic on Linux); if set, cancels all + refuses new for
   the rest of the session.
   **`check_force_liquidate()`** -- polls
   `kill_signals::force_liquidate_requested()` (SIGUSR2); on top of
   user-kill semantics, places marketable sells at best_bid for every
   open position. Operator commands: `kill -USR1` and `kill -USR2`.
6. **`ranking.step(t)`** -- updates Hawkes intensities, OU mu,
   hit_count tilt, computes `s.score`, fills `ranked_indices`.
7. **`step_trace` push** (when configured) -- per `step_trace_context_window`:
   either write the snapshot direct (legacy / trailing mode) or push
   to the ring buffer.
8. **`sync_next_order_id_from_broker()`** -- prevents collisions with
   the broker's own next-id when other clients touch the account.
9. **Entry loop** (skipped when `kill_switch_triggered_by_user_`): for
   each `ranked_indices[i]` while `i < top_k`:
   1. Skip if `s.cooldown > 0` or `!s.active`.
   2. **OU mu gate**: skip if `s.mid > s.ou.mu * (1 + ou_buy_threshold_pct)`.
      Mean-reversion entry: only buy when price is at or below recent mean.
   3. Skip if `s.best_limit <= 0` (no live L1 yet).
   4. Compute `target_notional` (per-symbol override or global `trade_notional`).
   5. `qty = size_entry_qty(limit, target_notional)`. Skip if 0
      (budget too small for one share, or legacy knobs zeroed it).
   6. Skip if `qty * limit > max_notional_per_order`.
   7. Skip if `max_orders_per_run` reached.
   8. Skip if `max_orders_per_symbol[sym]` reached.
   9. Skip if `max_open_symbols` reached.
   10. Skip if budget gate exhausted
       (`account_budget - open_notional < target_notional`).
   11. `emit_decision_snapshot` -> `broker_->place_limit_order` ->
       `emit_order_event("placed")` -> record in `entry_orders_` ->
       increment counters -> set cooldown -> mark
       `step_trace_event_this_step_ = true`.
10. **`step_trace` post-event** (when ring-buffer mode and a trade
    event fired): flush ring + arm trailing.
11. Heartbeat every 100 steps.

### Sell (exit) side -- inside `route_exit_orders()`

Called within `step()` after `refresh_order_state` (skipped when
`kill_switch_triggered_by_user_`). For each open position with
`sell_order_id == 0`:

1. `idx = portfolio_index_for_symbol(symbol)` -- skip if -1
   (shouldn't happen; defensive).
2. `ensure_depth_subscription(symbol, depth_ticker_id)` -- subscribes
   L2 lazily on first hit, so we don't carry the L2 cost for symbols
   we never open.
3. `book = broker_->snapshot_book(depth_ticker_id)`. Skip if
   `!has_valid_top(book)`.
4. `gross_target = entry_price * (1 + target_profit_pct)`.
5. `cost_per_share = estimate_round_trip_cost_per_share(qty, entry, gross_target)`.
6. **`sell_limit = max(gross_target + cost_per_share, current_bid)`** --
   the `max` clamp is the 2026-05-31 product-backlog item 1 fix:
   never post a sell below the current bid. Picked-up reconciled
   positions with stale `avg_cost` AND price spikes between buy fill
   and this step both benefit from this guard.
7. `mid = (best_bid + best_ask) / 2`.
8. `queue_ahead = visible_ask_queue_ahead(book, sell_limit)`.
9. `lambda_for_exit = max(hawkes_sell.lambda, hawkes.lambda, 1e-9)`.
10. `sell_score = compute_execution_score(mid, sell_limit,
    sell_directional_mu, lambda, queue_ahead, latency,
    net_reward, loss)`.
11. Emit L2 trace row.
12. Skip if `sell_score < min_sell_execution_score`.
13. Build sell `OrderRequest` (qty = position.qty, is_buy=false,
    limit=sell_limit, primary_exchange via per-symbol lookup).
14. `broker_->place_limit_order` -> `emit_order_event("placed")` ->
    record in `exit_order_symbols_` -> set `position.sell_order_id` ->
    mark `step_trace_event_this_step_ = true`.

### Sell terminal handling

Done in `refresh_order_state` on Filled / Cancelled status:

- **Filled exit**: `realized_pnl_ += (avg_fill_price - entry_price) * filled_qty`.
  Remove from `open_positions_`, `exit_order_symbols_`,
  `entry_orders_`.
- **Cancelled exit**: clear `sell_order_id` so the next step's
  `route_exit_orders` re-evaluates and places again (likely with a
  different sell_limit if the book has moved).

### Where the latency numbers come from

- `ack_latency_ms` -- placeOrder -> Submitted/PreSubmitted callback;
  measured by `IBKRClient`, stored under `event_mutex_`.
- `fill_latency_ms` -- placeOrder -> Filled callback; same mechanism.
  (Item 5 of the 2026-05-31 product backlog.)
- Per-side p50/p99/max in milliseconds end up in `metrics.json` (look
  for `buy_p50_ms`, `sell_p99_ms`, etc.) after `scripts/plot_run.py`
  joins placed + filled rows in `orders.csv` by `order_id`.

In backtest the broker fills synthetically -- placed and filled
share the same engine step, so the timestamp delta is microseconds.
For live + paper the same metrics reflect real broker round-trip,
which is operationally what we care about.

## Required handoff behavior

After each substantive user interaction, agents must append an entry to:

```text
AGENT_HANDOFF_LOG.md
```

A substantive interaction means the user asked for another modification, build/debug change, CI change, documentation change, script change, or repo/package update.

Do **not** add a handoff entry for:
- simple confirmations
- simple factual questions
- link-only answers
- brief explanations that do not change project state
- messages where no new decision, patch, or debugging result was produced

When in doubt, append a short handoff entry. The log should help the next agent continue from the latest project state.

## Open investigations: `#todo` and `#Done`

Some handoff entries describe an investigation or design follow-up that nobody
has acted on yet. Those entries carry a `#todo` tag in the title and/or in the
"Known risks / follow-up" section.

Every agent must:

1. On session start, scan `AGENT_HANDOFF_LOG.md` for entries tagged `#todo`
   and that are **not** tagged `#Done`. A short search like
   `grep -n "#todo" agent/AGENT_HANDOFF_LOG.md` is sufficient.
2. If at least one `#todo` looks relevant to the current task, surface it to
   the user and **ask whether to work on it now**. Do not start solving a
   `#todo` without explicit user approval — the entry may still be in design
   discussion, may conflict with the user's current direction, or may have
   blockers documented elsewhere.
3. When the user approves, solve the `#todo`. The fix lands as its own new
   handoff entry describing what was done (normal entry rules apply).
4. After the fix is committed (or otherwise accepted by the user), edit the
   **original** `#todo` entry's title to append `#Done` next to the existing
   `#todo` tag, and add a one-line back-reference to the resolving entry's
   date and title. Do not delete the original entry — keeping it preserves
   the investigation trail.

A `#todo` entry stays valid until it is explicitly retagged `#Done`. Once
`#Done`, future agents must skip it during the on-start scan; it remains in
the log only as historical context.

Title format for an open investigation:
```text
## [YYYY-MM-DD] - <short title> #todo
```

Title format after the investigation is resolved:
```text
## [YYYY-MM-DD] - <short title> #todo #Done (resolved by [YYYY-MM-DD] <resolver entry title>)
```

If a `#todo` is intentionally abandoned (decided against, superseded, no longer
relevant), retag it `#todo #Done` with a one-line note explaining why, so the
search-and-skip rule still works.

## Mandatory model and provider identity

Every handoff entry must include:
- the exact model name/version and model type used for that interaction
- the provider or client surface used to run it, such as Codex, Cursor, GitHub Copilot, Claude Code, ChatGPT web, OpenAI API, Anthropic API, or another web/API provider

For this interaction, the model identity is:

```text
GPT-5.5 Thinking, reasoning model
```

For this interaction, the provider/client identity is:

```text
Codex desktop
```

Do not write `unknown` for the model or provider/client field. Use `unknown` only for fields where the date, commit, provider/client, or source state is genuinely unavailable.

## Core rules for agents

1. Use the latest `main` commit as the source of truth whenever possible.
2. If direct cloning is unavailable, use the latest repo zip, raw GitHub file contents, or user-provided logs available in the current environment.
3. Never silently delete code or files. List deletions explicitly.
4. Keep scripts and generated files as valid multiline text files. Do not flatten YAML, Bash, CMake, C++, Markdown, or config files into one line.
5. **Run `./scripts/format_code.sh` after C++ edits and before staging the
   commit.** The script applies `.clang-format` to every `*.hpp`/`*.h`/
   `*.cpp` under `include/`, `src/`, and `tests/`. If the formatter
   modifies anything, re-`git add` those files before `git commit` so
   the commit includes the formatted version. On UCRT64 the binary is
   `/d/msys64/mingw64/bin/clang-format.exe` - prepend
   `/d/msys64/mingw64/bin` to `PATH` if `clang-format` isn't on yours.
   Linters/CI will flag unformatted C++; skipping this step costs a CI
   round-trip per missed file.
6. For any patch or generated artifact, state:
   - changed files
   - deletions/removals
   - validation performed
   - known risks
   - suggested commit message
7. For CI/build changes, explain the relationship between:
   - `third_party/` source trees
   - `dependencies/<toolchain>/install` dependency prefixes
   - project build directories such as `build-ucrt-ibkr`
8. For UCRT work, remember:
   - `third_party/` stores source code
   - `dependencies/ucrt64/install` stores compiled dependency outputs
   - `third_party/twsapi/client` supplies headers/sources for `twsapi_vendor`; root CMake builds it from source unless **`libtwsapi_vendor.a`** is already installed under **`CMAKE_PREFIX_PATH`** (Linux **`linux-deps`** bundle ships that prebuilt archive)
9. For Linux CI dependency work, remember:
   - `scripts/rebuild_linux_deps_ci.sh` builds `dependencies/linux/install`
   - it archives `dependencies/linux/linux-deps-ubuntu-latest.tar.gz`
   - CI can publish/download this archive as a `linux-deps` release asset

## Standard agent process

1. Inspect current project state.
   - Prefer latest `main`.
   - Otherwise use the latest uploaded/project zip.
2. Identify the failing step or requested change.
3. Patch the smallest set of files that fixes the issue.
4. Preserve existing project conventions unless the user explicitly asks to change them.
5. Validate what is possible in the current environment.
   - Examples:
     - `bash -n scripts/*.sh`
     - inspect YAML syntax
     - inspect generated CMake content
     - run dry-run paths if supported
     - run CMake configure/build only if dependencies are available
6. If any `*.hpp` / `*.h` / `*.cpp` under `include/`, `src/`, or `tests/`
   was touched, run `./scripts/format_code.sh` and re-stage anything the
   formatter modified, BEFORE creating the commit message. Commit and CI
   both expect formatted output.
7. Package the changed project or changed files if requested.
8. In the response, list:
   - changed files
   - deletions/removals
   - validation performed
   - known risks
   - suggested commit message
9. Append a handoff entry to `AGENT_HANDOFF_LOG.md` for substantive changes.

## Handoff entry template

Append new entries at the top of `AGENT_HANDOFF_LOG.md`.

```md
## [YYYY-MM-DD] - <short title>

Model / agent:
- Model: <exact model name/version, model type>
- Provider/client: <Codex, Cursor, GitHub Copilot, Claude Code, ChatGPT web, OpenAI API, Anthropic API, or other exact provider/client>
- Example model: GPT-5.5 Thinking, reasoning model
- Example provider/client: Codex desktop

Source state:
- <latest main commit, repo zip name, or raw files used>

User request:
- <brief request summary>

Files changed:
- `<path>` - <what changed>

Deletions / removals:
- <none, or list exact files/code blocks removed>

Steps taken:
1. <step>
2. <step>
3. <step>

Validation performed:
- <command or inspection>
- <result>

Known risks / follow-up:
- <risk or none>

Suggested commit:
```bash
git commit -m "<type(scope): summary>"
```
```

## Repository reference

```text
https://github.com/munteanu-mihai-alin/trading-system
```

## Important source locations

```text
include/
src/lib/
src/app/
tests/
third_party/
scripts/
.github/workflows/
```

## Important build output locations

```text
dependencies/ucrt64/install
dependencies/linux/install
build/
build-ucrt-ibkr/
build-ibkr-ci/
```

## Important scripts

```text
scripts/stage_third_party_sources_ucrt.sh
scripts/build_third_party_dependencies_ucrt.sh
scripts/rebuild_linux_deps_ci.sh
scripts/check_clang_format.sh
scripts/run_coverage_ci.sh
```

## CMake targets

```text
hft_lib
hft_app
hft_tests
twsapi_vendor
```

## CMake build dependencies

The build is unconditional; `protobuf::libprotobuf`, the Intel decimal runtime,
the vendored TWS API, `spdlog`, and `GTest`/`GMock` are mandatory and resolved
either via system packages, `CMAKE_PREFIX_PATH`, or vendored copies under
`third_party/`. There is no longer an IBKR on/off CMake option.
