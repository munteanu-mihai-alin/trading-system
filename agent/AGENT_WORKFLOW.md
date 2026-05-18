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
caches stay under `data/databento/` and are deliberately NOT copied into
per-run folders.

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
