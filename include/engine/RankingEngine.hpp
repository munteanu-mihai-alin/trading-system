#pragma once
#include <vector>

#include "bench/bench.hpp"
#include "core/portfolio.hpp"
#include "execution/fill_model.hpp"
#include "execution/score.hpp"
#include "io/logger.hpp"
#include "models/stock.hpp"
#include "sim/orderbook.hpp"
#include "sim/simulator.hpp"
#include "validation/validation.hpp"

namespace hft {

class RankingEngine {
  int live_top_k_;
  int shadow_top_k_ = 10;
  int top_k_;
  FillModel fill_model_;
  Simulator simulator_;
  OrderBook order_book_;
  Logger logger_;
  // When false, step() skips the synthetic shadow-portfolio block:
  // shadow_active stays unset, the per-step sine-wave PnL is not
  // computed or logged, and cooldown is not triggered. The code path
  // is left intact behind the gate so legacy HFT_TEST cases that
  // construct RankingEngine directly (default ctor arg true) keep
  // their existing behaviour.
  bool shadow_enabled_;
  // When false, step() skips the 8-candidate FillModel sweep against
  // the internal Simulator order_book_. best_limit defaults to s.mid
  // (broker decides fill against real L1/L2), score = hawkes.lambda
  // only. Bypassing the synthetic book avoids the O(N log N) sort cost
  // in match_at_price that wedges long-running backtests as the book
  // grows. Default true preserves legacy behaviour.
  bool synthetic_fill_model_;
  // Picks how `best_limit` is set when synthetic_fill_model_ is off.
  // "mid" (default) keeps the prior passive-at-mid behaviour; "ask"
  // sets best_limit to s.ask_price so the buy crosses immediately.
  // Backtest configs typically want "ask" so positions open and the
  // sell-side path actually runs.
  std::string entry_limit_mode_ = "mid";

  int my_id_counter_ = 100000;

 public:
  RankedPortfolio<Stock> portfolio;
  ValidationMetrics validation;
  std::vector<std::uint64_t> cycle_samples;

  explicit RankingEngine(int top_k, const std::string& csv_path,
                         bool shadow_enabled = true,
                         bool synthetic_fill_model = true,
                         std::string entry_limit_mode = "mid");
  [[nodiscard]] int live_top_k() const { return live_top_k_; }
  [[nodiscard]] int shadow_top_k() const { return shadow_top_k_; }
  [[nodiscard]] bool shadow_enabled() const { return shadow_enabled_; }
  [[nodiscard]] bool synthetic_fill_model() const {
    return synthetic_fill_model_;
  }

  void initialize(int n_stocks);
  void step(int t);
};

}  // namespace hft
