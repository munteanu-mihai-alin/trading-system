#pragma once

// Pure helper for picking the engine's outer-loop step count, factored
// out of main.cpp so the cap/override interaction is unit-testable.
//
// Why this exists at all: yen v2 (2026-05-29) wrote a 108 GB
// step_trace.csv and crashed the engine when /dev/sdb filled. Root
// cause was a one-line bug in main.cpp - `effective_steps` was
// OVERWRITTEN unconditionally by `broker->max_replay_steps()` when
// `steps_auto_from_broker=true`, even when the broker reported a
// MUCH bigger value than the configured `steps=` ceiling. The bug
// only surfaced after we switched the Yen window to Databento MBP-1
// L1 (~25 M rows for NVDA alone vs ~3 k rows for IBKR's 1-min bars
// for the SAME window). The 8.5 M `steps=` ceiling in the config was
// dead weight.
//
// The fix is to MIN with cfg.steps so the ceiling is honored. This
// header captures the semantics with a small pure function so any
// regression of the same logic shape gets caught by a unit test.
//
// See agent/AGENT_HANDOFF_LOG.md [2026-05-29] disk-full crash entry
// for the forensic walkthrough.

#include <algorithm>
#include <climits>  // INT_MAX

#include "config/AppConfig.hpp"

namespace hft {

// Returns the number of steps the engine outer loop should run.
//
// Semantics:
//   - `auto_from_broker=false`           -> always return cfg_steps.
//   - mode != DatabentoBacktest          -> return cfg_steps (live /
//     IBKR brokers report broker_max=0 anyway and we never want
//     to derive a step count from a real-time broker).
//   - broker_max <= 0                    -> return cfg_steps (broker
//     has nothing useful to say; preserve the configured value).
//   - Otherwise                          -> return min(cfg_steps,
//     broker_max). The broker may shorten the loop (we shouldn't
//     run past its data) but it must NEVER make us run longer than
//     the operator explicitly configured.
[[nodiscard]] inline int compute_effective_steps(int cfg_steps,
                                                 bool auto_from_broker,
                                                 BrokerMode mode,
                                                 int broker_max) {
  // steps<=0 means "run until stopped" for live/paper: return a very
  // large ceiling so the outer loop effectively runs until SIGTERM /
  // the app's Stop instead of exiting after a fixed count. Without this
  // a live/paper session ends the moment it finishes cfg.steps (e.g.
  // 8.5M steps completes in ~9 min on sparse paper data, which looked
  // like the engine "randomly stopping"). Excluded for DatabentoBacktest,
  // where a non-positive step count is a config error, not an infinite
  // run.
  if (cfg_steps <= 0 && mode != BrokerMode::DatabentoBacktest)
    return INT_MAX;
  if (!auto_from_broker)
    return cfg_steps;
  if (mode != BrokerMode::DatabentoBacktest)
    return cfg_steps;
  if (broker_max <= 0)
    return cfg_steps;
  return std::min(cfg_steps, broker_max);
}

}  // namespace hft
