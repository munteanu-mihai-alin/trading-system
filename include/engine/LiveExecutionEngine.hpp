#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "broker/IBroker.hpp"
#include "config/LiveTradingConfig.hpp"
#include "engine/RankingEngine.hpp"

namespace hft {

// Thin live layer that consumes ranked stocks and routes top-K orders to a broker.
// Shadow portfolio, validation, benchmarking, and simulator stay in RankingEngine.
class LiveExecutionEngine {
  LiveTradingConfig cfg_;
  std::unique_ptr<IBroker> broker_;
  int next_order_id_ = 1;
  int orders_placed_ = 0;
  std::unordered_map<std::string, int> symbol_order_counts_;

  struct EntryOrderState {
    std::string symbol;
    double qty = 0.0;
    double limit = 0.0;
  };

  struct OpenPositionState {
    std::string symbol;
    double qty = 0.0;
    double entry_price = 0.0;
    double entry_ack_latency_ms = 1.0;
    int sell_order_id = 0;
    double sell_limit = 0.0;
    double sell_score = 0.0;
  };

  std::unordered_map<int, EntryOrderState> entry_orders_;
  std::unordered_map<std::string, OpenPositionState> open_positions_;
  std::unordered_map<int, std::string> exit_order_symbols_;
  std::unordered_set<std::string> depth_subscribed_symbols_;

  [[nodiscard]] bool can_route_order(const Stock& stock) const;
  [[nodiscard]] bool has_open_exposure(const std::string& symbol) const;
  [[nodiscard]] int open_exposure_symbol_count() const;
  [[nodiscard]] bool can_open_new_exposure() const;
  // Sum of entry-order qty*limit (pending) and open-position qty*entry_price.
  // Used to enforce account_budget across symbols.
  [[nodiscard]] double committed_notional() const;
  // floor(target_notional / limit_price) when target_notional > 0; else
  // falls back to order_qty / max_order_qty semantics. Returns 0 when the
  // order should be skipped (price too high, qty rounds to 0, etc.). The
  // target_notional argument lets the caller select the rule: by default
  // it is trade_notional, but the score-weighted path passes a per-symbol
  // share of account_budget instead.
  [[nodiscard]] double size_entry_qty(double limit_price,
                                      double target_notional) const;
  // Per-symbol notional for this step, computed once per step before the
  // buy loop based on cfg_.app.position_sizing_rule and the currently
  // active items + their scores. Returns trade_notional for every symbol
  // when the rule is "equal" or when score_weighted has no positive score.
  [[nodiscard]] std::unordered_map<std::string, double>
  compute_per_symbol_notional() const;
  [[nodiscard]] bool sync_next_order_id_from_broker();
  [[nodiscard]] int portfolio_index_for_symbol(const std::string& symbol) const;
  [[nodiscard]] double allocated_daily_cost_per_share() const;
  [[nodiscard]] double estimate_round_trip_cost_per_share(
      double qty, double entry_price, double sell_price_estimate) const;
  void ensure_depth_subscription(const std::string& symbol, int ticker_id);
  void refresh_order_state();
  void route_exit_orders();
  // Per-step drain of broker trade prints into Stock::hawkes, gated on
  // cfg_.app.hawkes_use_real_trades.
  void update_hawkes_from_trades();
  // Per-step hit-count windowing on s.mid, gated on hit_count_enabled.
  // Counts windows of `horizon` seconds where mid moved up by at least
  // `target_pct`, then sets s.score_tilt from the running count.
  void update_hit_count_tilt();

 public:
  RankingEngine ranking;

  LiveExecutionEngine(LiveTradingConfig cfg, std::unique_ptr<IBroker> broker);

  bool start();
  void stop();

  void initialize_universe(int n_stocks);
  void subscribe_live_books(const std::vector<std::string>& symbols);
  void reconcile_broker_state();
  void step(int t);
};

}  // namespace hft
