#pragma once

#include "config/AppConfig.hpp"

// Pure helper for deciding how fast the engine's outer loop may spin,
// factored out of main.cpp so the live/backtest split is unit-testable.
//
// Why this exists: the live loop was
//
//     for (int t = 0; t < steps; ++t) { engine.step(t); }
//
// with no sleep anywhere. IBroker::on_step is a no-op default that
// IBKRClient never overrides, and nothing in Chronos2ExecutionEngine
// blocks, so in live/paper the loop is a pure spin. Measured on the
// Hetzner box it burned 8.5 M steps in ~9.3 s CPU (~900 k steps/sec)
// and then exited mid-session -- the engine appeared to "randomly
// stop". Setting steps=0 (INT_MAX) stops the early exit but leaves a
// hot loop pegging a core for the whole session.
//
// Pacing is pointless work anyway: IBKR throttles reqMktData to about
// 4 top-of-book snapshots per second (~250 ms), so stepping faster
// than that re-reads a book that cannot have changed.
//
// BACKTEST IS DIFFERENT AND MUST NEVER PACE. There step index t is the
// market-data row index (DatabentoBacktestBroker::on_step advances the
// replay cursor to row t), so a sleep per step would turn a few-second
// replay into days. The mode check is deliberately made here rather
// than relying on config, so a backtest run that inherits a live
// config cannot accidentally sleep its way through millions of rows.

namespace hft {

// Returns the per-step interval in milliseconds, or 0 for "run flat
// out". Backtests always return 0 regardless of the configured value.
[[nodiscard]] inline int effective_step_interval_ms(int cfg_interval_ms,
                                                    BrokerMode mode) {
  if (mode == BrokerMode::DatabentoBacktest)
    return 0;
  if (cfg_interval_ms <= 0)
    return 0;
  return cfg_interval_ms;
}

}  // namespace hft
