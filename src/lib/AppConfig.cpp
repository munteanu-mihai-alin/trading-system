#include "config/AppConfig.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace hft {

static std::string trim(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos)
    return "";
  const auto e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

static bool parse_bool(const std::string& s) {
  return s == "1" || s == "true" || s == "yes" || s == "on";
}

AppConfig AppConfig::load_from_file(const std::string& path) {
  AppConfig cfg{};
  std::ifstream in(path);

  if (!in.is_open()) {
    std::cerr << "Warning: could not open config file: " << path
              << ". Using defaults." << std::endl;
    return cfg;
  }

  std::string line;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#' || line[0] == '[')
      continue;

    const auto pos = line.find('=');
    if (pos == std::string::npos)
      continue;

    const auto key = trim(line.substr(0, pos));
    const auto val = trim(line.substr(pos + 1));

    try {
      if (key == "mode") {
        if (val == "live")
          cfg.mode = BrokerMode::Live;
        else if (val == "ibkr_paper" || val == "paper_ibkr")
          cfg.mode = BrokerMode::IBKRPaper;
        else if (val == "databento_backtest" || val == "backtest")
          cfg.mode = BrokerMode::DatabentoBacktest;
        else if (val == "sim")
          cfg.mode = BrokerMode::Sim;
        else
          cfg.mode = BrokerMode::Paper;
      } else if (key == "host") {
        cfg.host = val;
      } else if (key == "paper_port") {
        cfg.paper_port = std::stoi(val);
      } else if (key == "live_port") {
        cfg.live_port = std::stoi(val);
      } else if (key == "client_id") {
        cfg.client_id = std::stoi(val);
      } else if (key == "universe_size") {
        cfg.universe_size = std::stoi(val);
      } else if (key == "top_k") {
        cfg.top_k = std::stoi(val);
      } else if (key == "steps") {
        cfg.steps = std::stoi(val);
      } else if (key == "order_enabled") {
        cfg.order_enabled = parse_bool(val);
      } else if (key == "order_qty") {
        cfg.order_qty = std::stod(val);
      } else if (key == "max_order_qty") {
        cfg.max_order_qty = std::stod(val);
      } else if (key == "max_notional_per_order") {
        cfg.max_notional_per_order = std::stod(val);
      } else if (key == "trade_notional") {
        cfg.trade_notional = std::stod(val);
      } else if (key == "account_budget") {
        cfg.account_budget = std::stod(val);
      } else if (key == "position_sizing_rule") {
        cfg.position_sizing_rule = val;
      } else if (key == "decision_log_path") {
        cfg.decision_log_path = val;
      } else if (key == "order_log_path") {
        cfg.order_log_path = val;
      } else if (key == "step_trace_log_path") {
        cfg.step_trace_log_path = val;
      } else if (key == "l2_trace_log_path") {
        cfg.l2_trace_log_path = val;
      } else if (key == "log_append_mode") {
        cfg.log_append_mode = parse_bool(val);
      } else if (key == "run_label") {
        cfg.run_label = val;
      } else if (key == "symbol_universe_path") {
        cfg.symbol_universe_path = val;
      } else if (key == "shadow_enabled") {
        cfg.shadow_enabled = parse_bool(val);
      } else if (key == "synthetic_fill_model") {
        cfg.synthetic_fill_model = parse_bool(val);
      } else if (key == "entry_limit_mode") {
        cfg.entry_limit_mode = val;
      } else if (key == "steps_auto_from_broker") {
        cfg.steps_auto_from_broker = parse_bool(val);
      } else if (key == "max_open_symbols") {
        cfg.max_open_symbols = std::stoi(val);
      } else if (key == "max_orders_per_run") {
        cfg.max_orders_per_run = std::stoi(val);
      } else if (key == "max_orders_per_symbol") {
        cfg.max_orders_per_symbol = std::stoi(val);
      } else if (key == "target_profit_pct") {
        cfg.target_profit_pct = std::stod(val);
      } else if (key == "min_sell_execution_score") {
        cfg.min_sell_execution_score = std::stod(val);
      } else if (key == "commission_per_share") {
        cfg.commission_per_share = std::stod(val);
      } else if (key == "commission_min_per_order") {
        cfg.commission_min_per_order = std::stod(val);
      } else if (key == "half_spread_cost") {
        cfg.half_spread_cost = std::stod(val);
      } else if (key == "impact_coefficient") {
        cfg.impact_coefficient = std::stod(val);
      } else if (key == "assumed_daily_volume") {
        cfg.assumed_daily_volume = std::stod(val);
      } else if (key == "daily_energy_kwh") {
        cfg.daily_energy_kwh = std::stod(val);
      } else if (key == "energy_cost_per_kwh") {
        cfg.energy_cost_per_kwh = std::stod(val);
      } else if (key == "daily_inflation_cost") {
        cfg.daily_inflation_cost = std::stod(val);
      } else if (key == "expected_daily_shares") {
        cfg.expected_daily_shares = std::stod(val);
      } else if (key == "daily_loss_kill_usd") {
        cfg.daily_loss_kill_usd = std::stod(val);
      } else if (key == "trailing_stop_pct") {
        cfg.trailing_stop_pct = std::stod(val);
      } else if (key == "warmup_state_path") {
        cfg.warmup_state_path = val;
      } else if (key == "daily_loss_kill_alert_path") {
        cfg.daily_loss_kill_alert_path = val;
      } else if (key == "step_trace_context_window") {
        cfg.step_trace_context_window = std::stoi(val);
      } else if (key == "databento_cache_dir") {
        cfg.databento_cache_dir = val;
      } else if (key == "databento_python") {
        cfg.databento_python = val;
      } else if (key == "databento_l1_download_script" ||
                 key == "databento_download_mbp1_script") {
        cfg.databento_l1_download_script = val;
      } else if (key == "databento_l2_download_script" ||
                 key == "databento_download_script") {
        cfg.databento_l2_download_script = val;
      } else if (key == "databento_l1_dataset") {
        cfg.databento_l1_dataset = val;
      } else if (key == "databento_l2_dataset" || key == "databento_dataset") {
        cfg.databento_l2_dataset = val;
      } else if (key == "databento_l1_schema") {
        cfg.databento_l1_schema = val;
      } else if (key == "databento_l2_schema" || key == "databento_schema") {
        cfg.databento_l2_schema = val;
      } else if (key == "databento_start") {
        cfg.databento_start = val;
      } else if (key == "databento_end") {
        cfg.databento_end = val;

        // ---- Strategy dispatch ----
      } else if (key == "strategy_mode") {
        cfg.strategy_mode = val;

        // ---- Chronos-2 forecast bridge ----
      } else if (key == "chronos2_python") {
        cfg.chronos2_python = val;
      } else if (key == "chronos2_forecast_script") {
        cfg.chronos2_forecast_script = val;
      } else if (key == "chronos2_predictions_dir") {
        cfg.chronos2_predictions_dir = val;
      } else if (key == "chronos2_daily_closes_dir") {
        cfg.chronos2_daily_closes_dir = val;
      } else if (key == "chronos2_model") {
        cfg.chronos2_model = val;
      } else if (key == "chronos2_context_len") {
        cfg.chronos2_context_len = std::stoi(val);
      } else if (key == "chronos2_prediction_len") {
        cfg.chronos2_prediction_len = std::stoi(val);
      } else if (key == "chronos2_max_annual_vol") {
        cfg.chronos2_max_annual_vol = std::stod(val);
      } else if (key == "chronos2_vol_lookback_days") {
        cfg.chronos2_vol_lookback_days = std::stoi(val);
      } else if (key == "chronos2_momentum_lookback_days") {
        cfg.chronos2_momentum_lookback_days = std::stoi(val);
      } else if (key == "chronos2_vol_floor") {
        cfg.chronos2_vol_floor = std::stod(val);
      } else if (key == "chronos2_reinvest_increment") {
        cfg.chronos2_reinvest_increment = std::stod(val);
      }
    } catch (const std::exception& ex) {
      std::cerr << "Warning: invalid config entry '" << key << "'='" << val
                << "' in " << path << ": " << ex.what() << std::endl;
    }
  }

  return cfg;
}

}  // namespace hft
