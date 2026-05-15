#pragma once
#include <string>

namespace hft {

enum class BrokerMode { Paper, IBKRPaper, Live, Sim, DatabentoBacktest };

struct AppConfig {
  BrokerMode mode = BrokerMode::Paper;
  std::string host = "127.0.0.1";
  int paper_port = 7497;
  int live_port = 7496;
  int client_id = 1;
  int universe_size = 30;
  int top_k = 3;
  int steps = 500;
  bool order_enabled = true;
  double order_qty = 10.0;
  double max_order_qty = 10.0;
  double max_notional_per_order = 0.0;
  // Notional-driven sizing. When `trade_notional > 0`, the engine computes
  // entry qty as floor(trade_notional / limit_price) and ignores order_qty /
  // max_order_qty for that calc. `account_budget`, when > 0, caps the sum of
  // pending + open notional across all symbols. Both are in the order's
  // currency (USD for US equities).
  double trade_notional = 500.0;
  double account_budget = 1500.0;
  // Position-sizing rule. "equal" (default) uses trade_notional per active
  // symbol. "score_weighted" divides account_budget proportionally to each
  // active symbol's score (after ranking + tilt); symbols with non-positive
  // score get zero. "rank_weighted" allocates by rank position using
  // rank_weights below. score_weighted falls back to equal when total
  // active score is non-positive.
  std::string position_sizing_rule = "equal";
  // Ornstein-Uhlenbeck mean-reversion entry gate. Two parameterisations:
  //   - ou_halflife_seconds > 0 (preferred): dt-weighted EWMA where the
  //     half-life is in wall-clock seconds, consistent across symbols of
  //     different trade rates.
  //   - ou_window_size > 0 (legacy): sample-count EWMA where alpha =
  //     1/ou_window_size. Used only when ou_halflife_seconds <= 0.
  // Gate fires when ou_halflife_seconds > 0 OR ou_window_size > 0; the buy
  // path skips symbols where mid > ou.mu * (1 + ou_buy_threshold_pct).
  int ou_window_size = 0;
  double ou_halflife_seconds = 0.0;
  double ou_buy_threshold_pct = 0.0;
  // When true, LiveExecutionEngine subscribes to IBKR AllLast trade prints
  // for the live universe and drives the Stock::hawkes intensity from real
  // trade events (dt between consecutive trades on the symbol). When false,
  // Hawkes stays on RankingEngine's synthetic event clock. Default false
  // so existing tests and configs are unaffected.
  bool hawkes_use_real_trades = false;
  // Empirical "+target_pct hit-count" buy-side ranking tilt. Per-symbol
  // counter of historical price-increase windows that hit the target
  // return, multiplicatively tilting the ranking score. Disabled by
  // default; opt-in via hit_count_enabled. Horizon is the window length
  // in seconds; baseline is the hit-count divisor for the tilt; tilt
  // floors/ceilings clamp the resulting multiplier.
  bool hit_count_enabled = false;
  double hit_count_target_pct = 0.008;
  double hit_count_horizon_seconds = 60.0;
  double hit_count_baseline = 5.0;
  double hit_count_tilt_min = 1.0;
  double hit_count_tilt_max = 3.0;
  int max_open_symbols = 3;
  int max_orders_per_run = 0;
  int max_orders_per_symbol = 0;
  double target_profit_pct = 0.008;
  double min_sell_execution_score = 0.0;
  double commission_per_share = 0.005;
  double half_spread_cost = 0.0005;
  double impact_coefficient = 0.1;
  double assumed_daily_volume = 1'000'000.0;
  double daily_energy_kwh = 0.0;
  double energy_cost_per_kwh = 0.0;
  double daily_inflation_cost = 0.0;
  double expected_daily_shares = 1.0;
  std::string databento_cache_dir = "data/databento";
  std::string databento_python = "python";
  std::string databento_l1_download_script = "scripts/local_l1_csv_provider.py";
  std::string databento_l2_download_script = "scripts/databento_download_l2.py";
  std::string databento_l1_dataset = "data/l1";
  std::string databento_l2_dataset = "XNAS.ITCH";
  std::string databento_l1_schema = "mbp-1";
  std::string databento_l2_schema = "mbp-10";
  std::string databento_start;
  std::string databento_end;

  [[nodiscard]] int port() const noexcept {
    if (mode == BrokerMode::Live)
      return live_port;
    return paper_port;
  }

  static AppConfig load_from_file(const std::string& path);
};

}  // namespace hft
