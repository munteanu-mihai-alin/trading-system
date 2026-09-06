#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "broker/IBroker.hpp"
#include "config/LiveTradingConfig.hpp"
#include "core/portfolio.hpp"
#include "models/stock.hpp"

namespace hft {

// Chronos2ExecutionEngine -- forked routing engine for the
// "chronos2_mr_pred_exit" strategy. Does NOT share bookkeeping with
// LiveExecutionEngine; the two engines only agree on the IBroker
// interface. Selected in main.cpp via AppConfig::strategy_mode.
//
// Strategy port of research/quantconnect/Chronos2_MR_MultiFeature.py:
//   - Ranks by predicted_return / max(realised_vol, chronos2_vol_floor)
//     (Sharpe-like composite).
//   - Entry filters (all must pass):
//       predicted_return   >= target_profit_pct
//       predicted_q25_ret  >  0                       (confidence)
//       realized_vol       <= chronos2_max_annual_vol (crashy names)
//       momentum_5d        >= 0                       (falling knives)
//   - Sell target = max(predicted_price, entry*(1+target_profit_pct))
//     -- never sells at a loss (safety fallback).
//   - Reinvest schedule: every chronos2_reinvest_increment of realised
//     profit unlocks one more slot (increases effective account_budget
//     and effective top_k by one).
//
// Chronos-2 forecasts come from a Python subprocess spawned once per
// trading day (one-shot std::system call, same pattern as the
// Databento downloader).
class Chronos2ExecutionEngine {
 public:
  struct OpenPositionState {
    std::string symbol;
    double qty = 0.0;
    double entry_price = 0.0;
    int sell_order_id = 0;
    double sell_limit = 0.0;
    // Chronos-2 predicted price captured at fill time; used to hold the
    // Chronos-driven exit target even if a later daily forecast comes
    // back with a lower prediction.
    double predicted_price_at_fill = 0.0;
  };

  struct EntryOrderState {
    std::string symbol;
    double qty = 0.0;
    double limit = 0.0;
  };

  explicit Chronos2ExecutionEngine(LiveTradingConfig cfg,
                                   std::unique_ptr<IBroker> broker);
  ~Chronos2ExecutionEngine();

  bool start();
  void stop();
  void initialize_universe(
      const std::vector<std::pair<std::string, std::string>>& list,
      int n_stocks);
  void subscribe_live_books();
  void step(int t);

  // Diagnostics accessor for tests.
  [[nodiscard]] const std::unordered_map<std::string, OpenPositionState>&
  open_positions() const {
    return open_positions_;
  }
  [[nodiscard]] double realized_pnl() const { return realized_pnl_; }
  [[nodiscard]] double bonus_budget() const { return bonus_budget_; }

 private:
  // ---- Chronos-2 daily forecast bridge ----
  // Writes per-symbol daily-close history to a CSV, spawns Python
  // chronos2_forecast.py, reads predictions back into portfolio.items.
  void maybe_load_chronos_predictions();
  void write_daily_closes_csv(const std::string& out_path) const;
  int spawn_chronos_forecast(const std::string& in_csv,
                             const std::string& out_csv) const;
  void read_predictions_csv(const std::string& path);

  // ---- Scoring + routing ----
  void compute_composite_scores();
  void route_entries();
  void route_exit_orders();

  // ---- Order/position bookkeeping ----
  void reconcile_broker_state();
  void refresh_order_state();
  void handle_buy_fill(int order_id, double fill_price, double filled_qty);
  void handle_sell_fill(int order_id, double fill_price, double filled_qty);

  // ---- Reinvest schedule + capacity ----
  int effective_top_k() const;
  double effective_budget() const;
  double committed_notional() const;

  // ---- Helpers ----
  int portfolio_index_for_symbol(const std::string& symbol) const;
  void update_daily_close_history();

  LiveTradingConfig cfg_;
  std::unique_ptr<IBroker> broker_;

  // Universe state (mirrors RankingEngine's portfolio, but this engine
  // owns it directly rather than borrowing from RankingEngine).
  RankedPortfolio<Stock> portfolio_;

  // Order + position tracking.
  int next_order_id_ = 1;
  std::unordered_map<int, EntryOrderState> entry_orders_;
  std::unordered_map<std::string, OpenPositionState> open_positions_;
  std::unordered_map<int, std::string> exit_order_symbols_;

  // Rolling per-symbol daily-close history (used to build the Python
  // input CSV). Keyed by symbol; last N closes only, capped at
  // 2 * chronos2_context_len.
  std::unordered_map<std::string, std::vector<double>> daily_closes_;
  std::string last_load_yyyymmdd_;  // "2025-08-22" of the last successful
                                    // forecast load; forces a refresh
                                    // when the trading day rolls.

  double realized_pnl_ = 0.0;
  // Reinvest budget bump. Grows by chronos2_reinvest_increment for
  // every full increment of realized profit crossed. Never shrinks.
  double bonus_budget_ = 0.0;
  double next_reinvest_threshold_ =
      0.0;  // set = chronos2_reinvest_increment in start()

  bool started_ = false;
};

}  // namespace hft
