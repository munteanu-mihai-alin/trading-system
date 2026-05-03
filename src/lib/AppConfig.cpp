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
      }
    } catch (const std::exception& ex) {
      std::cerr << "Warning: invalid config entry '" << key << "'='" << val
                << "' in " << path << ": " << ex.what() << std::endl;
    }
  }

  return cfg;
}

}  // namespace hft
