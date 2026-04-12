#pragma once
#include <memory>
#include <string>
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
