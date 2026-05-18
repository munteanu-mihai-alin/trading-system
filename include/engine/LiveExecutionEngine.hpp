#pragma once
#include <fstream>
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
 public:
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

 private:
  LiveTradingConfig cfg_;
  std::unique_ptr<IBroker> broker_;
  int next_order_id_ = 1;
  int orders_placed_ = 0;
  std::unordered_map<std::string, int> symbol_order_counts_;

  std::unordered_map<int, EntryOrderState> entry_orders_;
  std::unordered_map<std::string, OpenPositionState> open_positions_;
  std::unordered_map<int, std::string> exit_order_symbols_;
  std::unordered_set<std::string> depth_subscribed_symbols_;
  // Backtest-only decision-trace writer. Opened in the constructor when
  // cfg_.app.decision_log_path is non-empty; nullptr otherwise. Each buy
  // event emits one CSV row per ranked symbol.
  std::unique_ptr<std::ofstream> decision_log_;
  int next_decision_id_ = 0;
  // Optional log: every order state change (placed/filled/cancelled/
  // rejected) for both buys and sells. Realized PnL is derived from
  // this file end-of-run.
  std::unique_ptr<std::ofstream> order_log_;
  // Optional log: per-engine-step ranking snapshot for all symbols.
  // Same schema as decision_log_ but fires every step, not just on
  // buys. Heavier - see AppConfig::step_trace_log_path docs.
  std::unique_ptr<std::ofstream> step_trace_log_;
  int next_step_trace_id_ = 0;
  // Optional log: per-step L2 microstructure trace for held positions.
  // Written from route_exit_orders.
  std::unique_ptr<std::ofstream> l2_trace_log_;
  // Current engine step, set at top of step(t). Lets refresh_order_state
  // and other helpers tag emitted rows with the right step without
  // threading t through every signature.
  int current_step_t_ = 0;

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
  // Writes one CSV row per ranked symbol on each buy event; no-op when
  // decision_log_ is null. The chosen_symbol arg tags which row is the
  // one that triggered this snapshot.
  void emit_decision_snapshot(int t, const std::string& chosen_symbol,
                              const std::string& gate);
  // Underlying writer used by both emit_decision_snapshot and the
  // per-step trace; factored so both files share the same 16-column
  // schema. decision_id is meaningful within a single output file
  // (each call increments). When chosen is empty, no row is marked
  // chosen=1.
  void write_ranking_snapshot_to(std::ofstream& out, int decision_id, int t,
                                 const std::string& chosen_symbol,
                                 const std::string& gate);
  // Order-lifecycle event (placed/filled/cancelled/rejected). No-op
  // when order_log_ is null. Uses current_step_t_ for the step column.
  void emit_order_event(int order_id, const std::string& symbol,
                        const std::string& side, double qty, double limit,
                        const std::string& event, double filled_qty,
                        double remaining_qty, double avg_fill_price);
  // Per-step L2 trace row. No-op when l2_trace_log_ is null.
  void emit_l2_trace(const std::string& symbol, const L2Book& book,
                     double sell_limit, double sell_score);

 public:
  RankingEngine ranking;

  LiveExecutionEngine(LiveTradingConfig cfg, std::unique_ptr<IBroker> broker);

  bool start();
  void stop();

  void initialize_universe(int n_stocks);
  void subscribe_live_books(const std::vector<std::string>& symbols);
  void reconcile_broker_state();
  void step(int t);

  // Per-symbol target notional for the current step. Computed once per step
  // inside step() and also exposed publicly so tests can verify the
  // allocation rule (position_sizing_rule). Returns trade_notional for every
  // active symbol when the rule is "equal" or when score_weighted has no
  // positive score.
  [[nodiscard]] std::unordered_map<std::string, double>
  compute_per_symbol_notional() const;

  // Read-only access to the engine's view of currently open positions.
  // Used by main.cpp's end-of-run summary to compute realised vs unrealised
  // PnL.
  [[nodiscard]] const std::unordered_map<std::string, OpenPositionState>&
  open_positions() const {
    return open_positions_;
  }
  [[nodiscard]] int orders_placed() const { return orders_placed_; }
};

}  // namespace hft
