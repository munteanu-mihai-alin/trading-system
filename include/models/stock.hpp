#pragma once
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
