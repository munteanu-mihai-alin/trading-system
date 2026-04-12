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
        out.use_real_ibkr = (cfg.mode == BrokerMode::Live);
        return out;
    }

    [[nodiscard]] std::string mode_name() const {
        if (app.mode == BrokerMode::Live) return "live";
        if (app.mode == BrokerMode::Sim) return "sim";
        return "paper";
    }
};

}  // namespace hft
