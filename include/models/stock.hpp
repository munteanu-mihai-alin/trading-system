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
  double mid = 100.0;
  double queue = 500.0;
  double best_limit = 100.0;
  double score = 0.0;

  Hawkes hawkes;
  OUState ou;
  LatencyModel latency;
  MyOrderState my_order;

  TradeStats real;
  TradeStats shadow;

  bool active = false;
  int cooldown = 0;
};

}  // namespace hft
