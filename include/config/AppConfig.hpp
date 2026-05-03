#pragma once
#include <string>

namespace hft {

enum class BrokerMode { Paper, IBKRPaper, Live, Sim };

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

  [[nodiscard]] int port() const noexcept {
    if (mode == BrokerMode::Live)
      return live_port;
    return paper_port;
  }

  static AppConfig load_from_file(const std::string& path);
};

}  // namespace hft
