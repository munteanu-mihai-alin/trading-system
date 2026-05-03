#pragma once
#include <string>

#include "config/AppConfig.hpp"

namespace hft {

struct LiveTradingConfig {
  AppConfig app;
  bool use_real_ibkr = false;

  static LiveTradingConfig from_app(const AppConfig& cfg) {
    LiveTradingConfig out;
    out.app = cfg;
    out.use_real_ibkr =
        (cfg.mode == BrokerMode::Live || cfg.mode == BrokerMode::IBKRPaper);
    return out;
  }

  [[nodiscard]] std::string mode_name() const {
    if (app.mode == BrokerMode::Live)
      return "live";
    if (app.mode == BrokerMode::IBKRPaper)
      return "ibkr_paper";
    if (app.mode == BrokerMode::Sim)
      return "sim";
    if (app.mode == BrokerMode::DatabentoBacktest)
      return "databento_backtest";
    return "paper";
  }
};

}  // namespace hft
