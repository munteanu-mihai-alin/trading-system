# Agent Workflow

This file defines the shared workflow for agents working on this repository. It should stay mostly stable. Active per-interaction notes belong in `AGENT_HANDOFF_LOG.md`.

## Purpose

The goal is to let multiple models or agents work on the same project without losing context or repeating mistakes. Agents should use this workflow to understand how to inspect the project, make changes, validate them, and hand off useful state to the next agent.

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

## Mandatory model identity

Every handoff entry must include the exact model name/version and model type used for that interaction.

For this interaction, the model identity is:

```text
GPT-5.5 Thinking, reasoning model
```

Do not write `unknown` for the model field. Use `unknown` only for fields where the date, commit, or source state is genuinely unavailable.

## Core rules for agents

1. Use the latest `main` commit as the source of truth whenever possible.
2. If direct cloning is unavailable, use the latest repo zip, raw GitHub file contents, or user-provided logs available in the current environment.
3. Never silently delete code or files. List deletions explicitly.
4. Keep scripts and generated files as valid multiline text files. Do not flatten YAML, Bash, CMake, C++, Markdown, or config files into one line.
5. For any patch or generated artifact, state:
   - changed files
   - deletions/removals
   - validation performed
   - known risks
   - suggested commit message
6. For CI/build changes, explain the relationship between:
   - `third_party/` source trees
   - `dependencies/<toolchain>/install` dependency prefixes
   - project build directories such as `build-ucrt-ibkr`
7. For UCRT work, remember:
   - `third_party/` stores source code
   - `dependencies/ucrt64/install` stores compiled dependency outputs
   - `third_party/twsapi/client` is source-only and is built by the root CMake as `twsapi_vendor`
8. For Linux CI dependency work, remember:
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
6. Package the changed project or changed files if requested.
7. In the response, list:
   - changed files
   - deletions/removals
   - validation performed
   - known risks
   - suggested commit message
8. Append a handoff entry to `AGENT_HANDOFF_LOG.md` for substantive changes.

## Handoff entry template

Append new entries at the top of `AGENT_HANDOFF_LOG.md`.

```md
## [YYYY-MM-DD] - <short title>

Model / agent:
- <exact model name/version, model type>
- Example: GPT-5.5 Thinking, reasoning model

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
scripts/generate_ci_workflow.sh
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

## Main CMake option

```text
HFT_ENABLE_IBKR
```
