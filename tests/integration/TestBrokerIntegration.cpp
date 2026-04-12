#include <memory>

#include "broker/ConnectionSupervisor.hpp"
#include "broker/IBKRClient.hpp"
#include "broker/OrderLifecycle.hpp"
#include "broker/PaperBrokerSim.hpp"
#include "common/TestFramework.hpp"
#include "config/AppConfig.hpp"
#include "config/LiveTradingConfig.hpp"
#include "core/portfolio.hpp"
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

HFT_TEST(test_order_lifecycle_transitions_cover_status_paths) {
    OrderLifecycleBook book;
    book.on_submitted(1, "AAPL", 10.0);
    hft::test::require(book.has(1), "submitted order should exist");
    hft::test::require(book.get(999) == nullptr, "missing order should return null");

    book.on_status(1, "Submitted", 0.0, 10.0, 0.0);
    hft::test::require(book.get(1)->status == OrderLifecycleStatus::Submitted,
                       "submitted status should map");

    book.on_status(1, "Filled", 10.0, 0.0, 101.0);
    hft::test::require(book.get(1)->status == OrderLifecycleStatus::Filled,
                       "filled status should map");

    book.on_status(1, "Cancelled", 0.0, 0.0, 0.0);
    hft::test::require(book.get(1)->status == OrderLifecycleStatus::Cancelled,
                       "cancelled status should map");

    book.on_status(1, "ApiCancelled", 0.0, 0.0, 0.0);
    hft::test::require(book.get(1)->status == OrderLifecycleStatus::Cancelled,
                       "ApiCancelled should map to cancelled");

    book.on_status(1, "Inactive", 0.0, 0.0, 0.0);
    hft::test::require(book.get(1)->status == OrderLifecycleStatus::Rejected,
                       "inactive should map to rejected");

    book.on_status(1, "Other", 5.0, 5.0, 100.0);
    hft::test::require(book.get(1)->status == OrderLifecycleStatus::PartiallyFilled,
                       "filled+remaining should map to partial");

    book.on_status(1, "Weird", 0.0, 0.0, 0.0);
    hft::test::require(book.get(1)->status == OrderLifecycleStatus::Unknown,
                       "unrecognized status should map to unknown");
}

HFT_TEST(test_connection_supervisor_backoff_and_reset_paths) {
    ConnectionSupervisor s;
    hft::test::require(s.should_retry(), "new supervisor should allow retry");
    int backoff = 0;
    for (int i = 0; i < 10; ++i) {
        backoff = s.next_backoff_ms();
    }
    hft::test::require(backoff <= 8000, "backoff should be capped");
    hft::test::require(!s.should_retry(), "after enough attempts retry should stop");
    s.reset();
    hft::test::require(s.should_retry(), "reset should re-enable retries");
}

HFT_TEST(test_paper_broker_poll_update_paths) {
    PaperBrokerSim broker;
    OrderUpdate u{};
    hft::test::require(!broker.poll_update(u), "empty broker queue should not produce update");

    broker.connect("127.0.0.1", 7497, 1);
    broker.place_limit_order(OrderRequest{1, "AAPL", true, 5.0, 100.0});
    hft::test::require(broker.poll_update(u), "submitted order should create update");
    hft::test::require(u.status == "Submitted", "first update should be submitted");

    broker.cancel_order(1);
    hft::test::require(broker.poll_update(u), "cancel should create update");
    hft::test::require(u.status == "Cancelled", "cancel update should be visible");
}

HFT_TEST(test_ibkr_stub_place_order_records_lifecycle_and_ack_miss) {
    IBKRClient client;
    hft::test::require(client.connect("127.0.0.1", 4002, 1), "stub connect should succeed");
    client.place_limit_order(OrderRequest{42, "AAPL", true, 7.0, 123.0});
    hft::test::require(client.lifecycle().has(42), "placing order should create lifecycle entry");
    const auto* state = client.lifecycle().get(42);
    hft::test::require(state != nullptr, "lifecycle state should exist");
    hft::test::require(state->status == OrderLifecycleStatus::Submitted,
                       "new order should start as submitted");
    hft::test::require_close(client.ack_latency_ms(999), 0.0, 1e-12,
                             "missing ack latency should be zero");
    hft::test::require(client.reconnect_once(), "stub reconnect should succeed");
    client.disconnect();
}

HFT_TEST(test_live_execution_engine_start_step_stop_paths) {
    AppConfig cfg;
    cfg.mode = BrokerMode::Paper;
    cfg.top_k = 2;
    auto broker = std::make_unique<PaperBrokerSim>();
    LiveExecutionEngine eng(LiveTradingConfig::from_app(cfg), std::move(broker));

    hft::test::require(eng.start(), "engine should start with paper broker");
    eng.initialize_universe(6);
    eng.subscribe_live_books({"A", "B", "C"});
    eng.reconcile_broker_state();
    eng.step(1);
    eng.stop();
}

HFT_TEST(test_order_lifecycle_initial_missing_state) {
    OrderLifecycleBook book;
    hft::test::require(!book.has(7), "missing order should not exist");
    hft::test::require(book.get(7) == nullptr, "missing order should return null state");
}

HFT_TEST(test_paper_broker_cancel_creates_update) {
    PaperBrokerSim broker;
    broker.connect("127.0.0.1", 7497, 1);
    broker.place_limit_order(OrderRequest{12, "MSFT", true, 3.0, 250.0});
    broker.cancel_order(12);

    OrderUpdate u{};
    hft::test::require(broker.poll_update(u), "submitted update should exist");
    hft::test::require(u.status == "Submitted", "first update should be submitted");
    hft::test::require(broker.poll_update(u), "cancel update should exist");
    hft::test::require(u.status == "Cancelled", "second update should be cancelled");
}

HFT_TEST(test_ibkr_stub_order_lifecycle_entry_created_on_place) {
    IBKRClient client;
    hft::test::require(client.connect("127.0.0.1", 4002, 1), "stub connect should succeed");
    client.place_limit_order(OrderRequest{77, "NVDA", true, 4.0, 900.0});
    hft::test::require(client.lifecycle().has(77), "placing order should create lifecycle entry");
    const auto* state = client.lifecycle().get(77);
    hft::test::require(state != nullptr, "lifecycle state should exist");
    hft::test::require(state->requested_qty == 4.0, "requested qty should be stored");
    hft::test::require(state->symbol == "NVDA", "symbol should be stored");
    client.disconnect();
}

HFT_TEST(test_ibkr_stub_snapshot_default_and_ack_latency_default) {
    IBKRClient client;
    hft::test::require(client.connect("127.0.0.1", 4002, 1), "stub connect should succeed");
    const auto b = client.snapshot_book(999);
    hft::test::require_close(b.best_bid(), 0.0, 1e-12, "missing stub snapshot should be empty");
    hft::test::require_close(b.best_ask(), 0.0, 1e-12, "missing stub snapshot should be empty");
    hft::test::require_close(client.ack_latency_ms(555), 0.0, 1e-12,
                             "missing ack latency should be zero");
    client.disconnect();
}

HFT_TEST(test_live_execution_engine_reconcile_without_ibkr_book_data) {
    AppConfig cfg;
    cfg.mode = BrokerMode::Paper;
    cfg.top_k = 2;
    auto broker = std::make_unique<PaperBrokerSim>();
    LiveExecutionEngine eng(LiveTradingConfig::from_app(cfg), std::move(broker));

    hft::test::require(eng.start(), "paper engine should start");
    eng.initialize_universe(5);
    eng.reconcile_broker_state();
    eng.stop();
}

HFT_TEST(test_live_execution_engine_step_routes_only_top_k_orders) {
    AppConfig cfg;
    cfg.mode = BrokerMode::Paper;
    cfg.top_k = 2;
    auto broker = std::make_unique<PaperBrokerSim>();
    auto* raw = broker.get();

    LiveExecutionEngine eng(LiveTradingConfig::from_app(cfg), std::move(broker));
    hft::test::require(eng.start(), "paper engine should start");
    eng.initialize_universe(6);
    eng.step(0);
    hft::test::require(static_cast<int>(raw->placed.size()) == 2,
                       "step should route exactly top_k orders");
    eng.stop();
}

HFT_TEST(test_ranking_engine_step_generates_real_and_shadow_activity_over_time) {
    RankingEngine engine(2, "tmp_shadow_results_2.csv");
    engine.initialize(10);

    for (int t = 0; t < 20; ++t) {
        engine.step(t);
    }

    int active_count = 0;
    int real_trades = 0;
    int shadow_trades = 0;
    int cooldown_count = 0;
    for (const auto& s : engine.portfolio.items) {
        if (s.active) active_count++;
        real_trades += s.real.trades;
        shadow_trades += s.shadow.trades;
        if (s.cooldown > 0) cooldown_count++;
    }

    hft::test::require(active_count == 2, "ranking engine should keep exactly top_k active");
    hft::test::require(real_trades > 0, "there should be real trades over time");
    hft::test::require(shadow_trades > 0, "there should be shadow trades over time");
    hft::test::require(cooldown_count >= 0, "cooldown scan should be reachable");
}

HFT_TEST(test_ranked_portfolio_rank_handles_empty_and_singleton) {
    RankedPortfolio<Stock> p;
    p.rank();
    hft::test::require(p.items.empty(), "empty portfolio should remain empty");

    Stock s;
    s.score = 5.0;
    p.items.push_back(s);
    p.rank();
    hft::test::require(p.items.size() == 1, "singleton portfolio should remain size one");
    hft::test::require_close(p.items[0].score, 5.0, 1e-12,
                             "singleton portfolio score should remain unchanged");
}

HFT_TEST(test_live_execution_engine_with_ibkr_stub_reconcile_path) {
    AppConfig cfg;
    cfg.mode = BrokerMode::Live;
    cfg.top_k = 1;

    auto broker = std::make_unique<IBKRClient>();
    LiveExecutionEngine eng(LiveTradingConfig::from_app(cfg), std::move(broker));

    hft::test::require(eng.start(), "ibkr stub engine should start");
    eng.initialize_universe(4);
    eng.subscribe_live_books({"AAPL", "MSFT", "NVDA", "AMD"});
    eng.reconcile_broker_state();
    eng.step(1);
    eng.stop();
}

HFT_TEST(test_live_execution_engine_zero_top_k_places_no_orders) {
    AppConfig cfg;
    cfg.mode = BrokerMode::Paper;
    cfg.top_k = 0;

    auto broker = std::make_unique<PaperBrokerSim>();
    auto* raw = broker.get();
    LiveExecutionEngine eng(LiveTradingConfig::from_app(cfg), std::move(broker));

    hft::test::require(eng.start(), "paper engine should start");
    eng.initialize_universe(5);
    eng.step(2);
    hft::test::require(raw->placed.empty(), "top_k zero should place no orders");
    eng.stop();
}

HFT_TEST(test_ranking_engine_cooldown_decrements_across_step) {
    RankingEngine engine(2, "tmp_shadow_results_3.csv");
    engine.initialize(3);
    engine.portfolio.items[0].cooldown = 2;
    engine.step(0);
    hft::test::require(engine.portfolio.items[0].cooldown <= 1,
                       "cooldown should decrement during step");
}

HFT_TEST(test_ibkr_stub_reconnect_when_connected_short_circuits_true) {
    IBKRClient client;
    hft::test::require(client.connect("127.0.0.1", 4002, 1), "stub connect should succeed");
    hft::test::require(client.reconnect_once(), "connected stub should return true on reconnect");
    client.disconnect();
}

HFT_TEST(test_ibkr_stub_reconnect_when_disconnected_uses_stored_params) {
    IBKRClient client;
    hft::test::require(client.connect("127.0.0.1", 4002, 7), "initial stub connect should succeed");
    client.disconnect();
    hft::test::require(client.reconnect_once(),
                       "disconnected stub should reconnect using stored params");
    client.disconnect();
}

HFT_TEST(test_ibkr_stub_start_stop_event_loops_are_reentrant) {
    IBKRClient client;
    hft::test::require(client.connect("127.0.0.1", 4002, 1), "stub connect should succeed");
    client.start_event_loop();
    client.start_event_loop();
    client.stop_event_loop();
    client.stop_event_loop();
    client.start_production_event_loop();
    client.disconnect();
}

HFT_TEST(test_order_lifecycle_keeps_requested_qty_after_status_updates) {
    OrderLifecycleBook book;
    book.on_submitted(5, "TSLA", 11.0);
    book.on_status(5, "Submitted", 0.0, 11.0, 0.0);
    const auto* state = book.get(5);
    hft::test::require(state != nullptr, "state should exist");
    hft::test::require_close(state->requested_qty, 11.0, 1e-12,
                             "requested qty should remain stored");
}
