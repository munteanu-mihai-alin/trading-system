#pragma once
#include <cstdint>
#include <string>

#include "execution/latency_model.hpp"
#include "models/trade.hpp"
#include "sim/queue_tracker.hpp"

namespace hft {

struct Stock {
  std::string symbol;
  std::string company;
  double mid = 100.0;
  // Best bid / ask copied from the broker's top-of-book snapshot in
  // Chronos2ExecutionEngine::reconcile_broker_state. Zero until the
  // first valid TopOfBook arrives; the mid is derived from these.
  double bid_price = 0.0;
  double ask_price = 0.0;
  double queue = 500.0;
  double best_limit = 100.0;
  double score = 0.0;

  LatencyModel latency;
  MyOrderState my_order;

  TradeStats real;
  TradeStats shadow;

  bool active = false;
  bool shadow_active = false;
  int cooldown = 0;

  // ==== Chronos-2 forecasting state ====
  // Populated by Chronos2ExecutionEngine::load_chronos_predictions()
  // once per trading day (from a Python subprocess that runs the
  // pretrained pfnet/Amazon Chronos-2 model). Ignored by the default
  // Hawkes+OU engine.
  //
  // predicted_price: mean forecast of next-day close.
  // predicted_q25:   25th-percentile forecast (confidence check).
  // realized_vol:    annualised std of daily log returns over
  //                  chronos2_vol_lookback_days.
  // momentum_5d:     (last_close - close_5d_ago) / close_5d_ago.
  double predicted_price = 0.0;
  double predicted_q25 = 0.0;
  double realized_vol = 0.0;
  double momentum_5d = 0.0;
};

}  // namespace hft
