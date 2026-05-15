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
  double queue = 500.0;
  double best_limit = 100.0;
  double score = 0.0;

  Hawkes hawkes;
  // Venue timestamp of the last consumed trade event, in Unix nanoseconds.
  // 0 means "no trade observed yet"; the engine uses a small default dt
  // (~1 ms) for the very first event to avoid Hawkes decaying to baseline
  // on huge first-event dt.
  std::int64_t last_trade_ts_ns = 0;
  OUState ou;
  // Set on the first real observation that primes ou.mu away from its
  // default value; otherwise the mean-reversion gate would block buys on
  // any symbol whose actual price differs from OUState's default mu=100
  // (e.g. $250 AAPL) until the EWMA has converged.
  bool ou_initialized = false;
  LatencyModel latency;
  MyOrderState my_order;

  TradeStats real;
  TradeStats shadow;

  bool active = false;
  bool shadow_active = false;
  int cooldown = 0;
};

}  // namespace hft
