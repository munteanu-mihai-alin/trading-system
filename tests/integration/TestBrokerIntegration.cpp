#include "common/TestFramework.hpp"

#include <memory>

#include "broker/PaperBrokerSim.hpp"
#include "config/AppConfig.hpp"
#include "config/LiveTradingConfig.hpp"
#include "engine/LiveExecutionEngine.hpp"

using namespace hft;

HFT_TEST(test_paper_broker_receives_orders) {
    AppConfig cfg;
    cfg.mode = BrokerMode::Paper;
    cfg.top_k = 3;
    cfg.steps = 1;

    auto broker = std::make_unique<PaperBrokerSim>();
    auto* raw = broker.get();

    LiveExecutionEngine eng(LiveTradingConfig::from_app(cfg), std::move(broker));
    hft::test::require(eng.start(), "paper broker should connect");
    eng.initialize_universe(10);
    eng.step(0);
    eng.stop();

    hft::test::require(!raw->placed.empty(), "paper broker should receive placed orders");
}

HFT_TEST(test_live_config_maps_mode_name) {
    AppConfig cfg;
    cfg.mode = BrokerMode::Live;
    const auto live = LiveTradingConfig::from_app(cfg);
    hft::test::require(live.use_real_ibkr, "live config should enable real ibkr");
    hft::test::require(live.mode_name() == "live", "mode name should be live");
}
