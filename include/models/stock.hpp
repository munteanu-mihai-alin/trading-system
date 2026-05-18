#pragma once
#include <cstdint>
#include <string>

#include "execution/latency_model.hpp"
#include "models/hawkes.hpp"
#include "models/ou.hpp"
#include "models/trade.hpp"
#include "sim/queue_tracker.hpp"

namespace hft {

struct Stock {
  std::string symbol;
  std::string company;
  double mid = 100.0;
  // Best bid / ask copied from the broker's top-of-book snapshot in
  // LiveExecutionEngine::reconcile_broker_state. Zero until the first
  // valid TopOfBook arrives. Used by RankingEngine to set best_limit
  // realistically (marketable at ask, passive at mid, etc.) under the
  // AppConfig::entry_limit_mode knob.
  double bid_price = 0.0;
  double ask_price = 0.0;
  double queue = 500.0;
  double best_limit = 100.0;
  double score = 0.0;

  // Buy-aggressor activity (existing field; kept under its original name
  // for backward compatibility with code that reads `s.hawkes.lambda`).
  Hawkes hawkes;
  // Sell-aggressor activity. Driven by trades classified as sell-initiated
  // (price <= best_bid) via Lee-Ready in update_hawkes_from_trades. Used
  // by route_exit_orders as the activity weight in compute_execution_score.
  Hawkes hawkes_sell;
  // Venue timestamp of the last consumed trade event for EITHER channel,
  // in Unix nanoseconds. 0 means "no trade observed yet"; the engine uses
  // a small default dt (~1 ms) for the very first event to avoid Hawkes
  // decaying to baseline on huge first-event dt.
  std::int64_t last_trade_ts_ns = 0;
  // Mid at the time of the last L1-mid-change-driven Hawkes event. Used
  // by AppConfig::hawkes_mid_change_threshold_bps to fire a Hawkes
  // event only on significant moves rather than on every observation.
  // Zero until the first valid mid.
  double last_mid_for_hawkes = 0.0;
  // Hit-count state for the empirical "+target_pct" buy-side ranking tilt.
  // window_open_mid = mid at the start of the current window; ts_ns = when
  // the window opened (steady-clock nanoseconds); hit_count = number of
  // windows that hit the target since the engine started. score_tilt is
  // the resulting multiplier applied by RankingEngine to the raw score
  // (1.0 = neutral; >1 promotes; <1 demotes).
  double window_open_mid = 0.0;
  std::int64_t window_open_ts_ns = 0;
  int hit_count = 0;
  double score_tilt = 1.0;
  OUState ou;
  // Set on the first real observation that primes ou.mu away from its
  // default value; otherwise the mean-reversion gate would block buys on
  // any symbol whose actual price differs from OUState's default mu=100
  // (e.g. $250 AAPL) until the EWMA has converged.
  bool ou_initialized = false;
  // steady-clock nanoseconds of the last OU update on this symbol, used
  // when AppConfig::ou_halflife_seconds > 0 to weight the EWMA by dt.
  std::int64_t last_ou_update_ts_ns = 0;
  LatencyModel latency;
  MyOrderState my_order;

  TradeStats real;
  TradeStats shadow;

  bool active = false;
  bool shadow_active = false;
  int cooldown = 0;
};

}  // namespace hft
