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
  // Ornstein-Uhlenbeck mean-reversion entry gate. When `ou_window_size > 0`,
  // each ranking-step update advances Stock::ou.mu by a slow EWMA of the
  // observed mid (effective half-life ~ ou_window_size * ln 2 samples), and
  // the buy path skips symbols where mid > ou.mu * (1 + ou_buy_threshold_pct)
  // - i.e. "don't buy stocks priced above their trailing mean by more than
  // the threshold." Set ou_window_size = 0 to disable the gate entirely.
  int ou_window_size = 0;
  double ou_buy_threshold_pct = 0.0;
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
