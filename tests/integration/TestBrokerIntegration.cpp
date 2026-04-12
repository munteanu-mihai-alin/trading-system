#include <memory>

#include "broker/IBKRClient.hpp"
#include "broker/PaperBrokerSim.hpp"
#include "common/TestFramework.hpp"
#include "config/AppConfig.hpp"
#include "config/LiveTradingConfig.hpp"
#include "engine/LiveExecutionEngine.hpp"
#include "engine/RankingEngine.hpp"

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

HFT_TEST(test_paper_broker_supports_event_loop_and_depth_subscribe) {
    PaperBrokerSim broker;
    hft::test::require(broker.connect("127.0.0.1", 7497, 1), "paper broker should connect");
    broker.start_event_loop();
    broker.subscribe_market_depth(MarketDepthRequest{1, "AAPL", 5});
    broker.stop_event_loop();
    broker.disconnect();
    hft::test::require(!broker.is_connected(), "paper broker should disconnect cleanly");
}

HFT_TEST(test_ibkr_stub_snapshot_and_reconnect_interfaces) {
    IBKRClient client;
    hft::test::require(client.connect("127.0.0.1", 4002, 1), "stub ibkr connect should succeed");
    client.subscribe_market_depth(MarketDepthRequest{1, "AAPL", 5});
    const auto book = client.snapshot_book(1);
    hft::test::require(book.best_bid() == 0.0, "stub snapshot should be empty without sdk");
    client.start_event_loop();
    client.start_production_event_loop();
    client.stop_event_loop();
    client.disconnect();
    hft::test::require(!client.is_connected(), "stub ibkr should disconnect");
}

// ===== Branch coverage cases =====

HFT_TEST(test_ranking_engine_initialize_and_step_paths) {
    RankingEngine engine(3, "tmp_shadow_results.csv");
    engine.initialize(8);
    hft::test::require(engine.portfolio.items.size() == 8,
                       "engine should initialize requested universe size");

    for (int t = 0; t < 5; ++t) {
        engine.step(t);
    }

    hft::test::require(!engine.cycle_samples.empty(), "engine should record cycle samples");
    hft::test::require(engine.validation.size() > 0, "engine should accumulate validation samples");
}
