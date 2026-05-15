// Unit tests for hft::LiveExecutionEngine driven by a gmock IBroker double.
//
// Covers the engine's broker-facing surface: start/stop wiring,
// initialize_universe forwarding to the RankingEngine, subscribe_live_books
// fan-out, step()'s order-placement loop, and reconcile_broker_state's
// dynamic_cast bail-out for non-IBKRClient brokers.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "common/MockIBroker.hpp"
#include "config/AppConfig.hpp"
#include "config/LiveTradingConfig.hpp"
#include "engine/LiveExecutionEngine.hpp"

namespace {

using ::testing::_;
using ::testing::AtLeast;
using ::testing::NiceMock;
using ::testing::Return;

namespace hft_test = hft::test;

hft::LiveTradingConfig make_paper_config(int top_k = 3) {
  hft::AppConfig app;
  app.mode = hft::BrokerMode::Paper;
  app.top_k = top_k;
  app.steps = 1;
  return hft::LiveTradingConfig::from_app(app);
}

TEST(LiveExecutionEngine, ConstructorDoesNotConnect) {
  auto broker = std::make_unique<NiceMock<hft_test::MockIBroker>>();
  // Constructor must not touch the broker.
  EXPECT_CALL(*broker, connect(_, _, _)).Times(0);
  EXPECT_CALL(*broker, disconnect()).Times(0);
  hft::LiveExecutionEngine engine(make_paper_config(), std::move(broker));
}

TEST(LiveExecutionEngine, StartCallsBrokerConnectAndReturnsTrue) {
  auto broker = std::make_unique<NiceMock<hft_test::MockIBroker>>();
  EXPECT_CALL(*broker, connect("127.0.0.1", 7497, 1)).WillOnce(Return(true));
  hft::LiveExecutionEngine engine(make_paper_config(), std::move(broker));
  EXPECT_TRUE(engine.start());
}

TEST(LiveExecutionEngine, StartReturnsFalseOnConnectFailure) {
  auto broker = std::make_unique<NiceMock<hft_test::MockIBroker>>();
  EXPECT_CALL(*broker, connect(_, _, _)).WillOnce(Return(false));
  hft::LiveExecutionEngine engine(make_paper_config(), std::move(broker));
  EXPECT_FALSE(engine.start());
}

TEST(LiveExecutionEngine, IBKRPaperUsesPaperPortAndRealIBKRMode) {
  hft::AppConfig app;
  app.mode = hft::BrokerMode::IBKRPaper;
  app.paper_port = 4002;
  app.client_id = 44;
  const auto cfg = hft::LiveTradingConfig::from_app(app);
  EXPECT_TRUE(cfg.use_real_ibkr);
  EXPECT_EQ(cfg.mode_name(), "ibkr_paper");

  auto broker = std::make_unique<NiceMock<hft_test::MockIBroker>>();
  EXPECT_CALL(*broker, connect("127.0.0.1", 4002, 44)).WillOnce(Return(true));
  hft::LiveExecutionEngine engine(cfg, std::move(broker));
  EXPECT_TRUE(engine.start());
}

TEST(LiveExecutionEngine, IBKRPaperUsesConfiguredPaperPortWithoutSpecialPath) {
  hft::AppConfig app;
  app.mode = hft::BrokerMode::IBKRPaper;
  app.paper_port = 4001;
  app.client_id = 45;
  const auto cfg = hft::LiveTradingConfig::from_app(app);

  auto broker = std::make_unique<NiceMock<hft_test::MockIBroker>>();
  EXPECT_CALL(*broker, connect("127.0.0.1", 4001, 45)).WillOnce(Return(true));
  hft::LiveExecutionEngine engine(cfg, std::move(broker));
  EXPECT_TRUE(engine.start());
}

TEST(LiveExecutionEngine, StopCallsBrokerDisconnect) {
  auto broker = std::make_unique<NiceMock<hft_test::MockIBroker>>();
  EXPECT_CALL(*broker, disconnect()).Times(1);
  hft::LiveExecutionEngine engine(make_paper_config(), std::move(broker));
  engine.stop();
}

TEST(LiveExecutionEngine, InitializeUniversePopulatesRanking) {
  auto broker = std::make_unique<NiceMock<hft_test::MockIBroker>>();
  hft::LiveExecutionEngine engine(make_paper_config(), std::move(broker));
  engine.initialize_universe(8);
  EXPECT_EQ(engine.ranking.portfolio.items.size(), 8u);
}

TEST(LiveExecutionEngine, SubscribeLiveBooksFansOutToBroker) {
  auto broker = std::make_unique<NiceMock<hft_test::MockIBroker>>();
  EXPECT_CALL(*broker, subscribe_top_of_book(_)).Times(3);
  EXPECT_CALL(*broker, subscribe_market_depth(_)).Times(0);
  hft::LiveExecutionEngine engine(make_paper_config(), std::move(broker));
  engine.subscribe_live_books({"AAPL", "MSFT", "GOOG"});
}

TEST(LiveExecutionEngine, SubscribeLiveBooksHandlesEmptyList) {
  auto broker = std::make_unique<NiceMock<hft_test::MockIBroker>>();
  EXPECT_CALL(*broker, subscribe_top_of_book(_)).Times(0);
  EXPECT_CALL(*broker, subscribe_market_depth(_)).Times(0);
  hft::LiveExecutionEngine engine(make_paper_config(), std::move(broker));
  engine.subscribe_live_books({});
}

TEST(LiveExecutionEngine, StepPlacesOrdersForActivePortfolioItems) {
  auto broker = std::make_unique<NiceMock<hft_test::MockIBroker>>();
  // After initialize_universe + a single step, the ranking engine selects up
  // to top_k active items. We don't pin a hard count - just confirm at least
  // one order was placed and that nothing else broker-side blew up.
  EXPECT_CALL(*broker, place_limit_order(_)).Times(AtLeast(1));
  hft::LiveExecutionEngine engine(make_paper_config(/*top_k=*/3),
                                  std::move(broker));
  engine.initialize_universe(10);
  engine.step(0);
}

TEST(LiveExecutionEngine, StepHonorsOrderDisabledConfig) {
  auto broker = std::make_unique<NiceMock<hft_test::MockIBroker>>();
  EXPECT_CALL(*broker, place_limit_order(_)).Times(0);
  auto cfg = make_paper_config(/*top_k=*/3);
  cfg.app.order_enabled = false;
  hft::LiveExecutionEngine engine(cfg, std::move(broker));
  engine.initialize_universe(10);
  engine.step(0);
}

TEST(LiveExecutionEngine, StepHonorsMaxOrdersPerRun) {
  auto broker = std::make_unique<NiceMock<hft_test::MockIBroker>>();
  EXPECT_CALL(*broker, place_limit_order(_)).Times(1);
  auto cfg = make_paper_config(/*top_k=*/3);
  cfg.app.max_orders_per_run = 1;
  hft::LiveExecutionEngine engine(cfg, std::move(broker));
  engine.initialize_universe(10);
  engine.step(0);
  engine.step(1);
}

TEST(LiveExecutionEngine, StepHonorsMaxOrdersPerSymbol) {
  auto broker = std::make_unique<NiceMock<hft_test::MockIBroker>>();
  EXPECT_CALL(*broker, place_limit_order(_)).Times(1);
  auto cfg = make_paper_config(/*top_k=*/1);
  cfg.app.max_orders_per_symbol = 1;
  hft::LiveExecutionEngine engine(cfg, std::move(broker));
  engine.initialize_universe(1);
  engine.step(0);
  engine.step(1);
}

TEST(LiveExecutionEngine, ReconcileBrokerStateReadsTopOfBookOnly) {
  auto broker = std::make_unique<NiceMock<hft_test::MockIBroker>>();
  EXPECT_CALL(*broker, place_limit_order(_)).Times(0);
  EXPECT_CALL(*broker, subscribe_market_depth(_)).Times(0);
  EXPECT_CALL(*broker, snapshot_top_of_book(_)).Times(5);
  hft::LiveExecutionEngine engine(make_paper_config(), std::move(broker));
  engine.initialize_universe(5);
  engine.reconcile_broker_state();
}

TEST(LiveExecutionEngine, BudgetGateLimitsBuysToAccountBudget) {
  // With trade_notional=$500 and account_budget=$1500 on stocks priced ~$100,
  // each buy commits ~$500 of notional and the budget allows exactly three
  // concurrent buys, regardless of how many ranked items are active or how
  // permissive max_open_symbols is. This isolates the budget gate from the
  // max_open_symbols gate (set to 10 so it doesn't fire first).
  auto broker = std::make_unique<NiceMock<hft_test::MockIBroker>>();
  EXPECT_CALL(*broker, place_limit_order(_)).Times(3);

  hft::AppConfig app;
  app.mode = hft::BrokerMode::Paper;
  app.top_k = 10;
  app.steps = 1;
  app.trade_notional = 500.0;
  app.account_budget = 1500.0;
  app.max_open_symbols = 10;  // disable the per-symbol-count gate
  app.max_orders_per_run = 0;
  app.max_orders_per_symbol = 0;
  hft::LiveExecutionEngine engine(hft::LiveTradingConfig::from_app(app),
                                  std::move(broker));
  engine.initialize_universe(10);
  engine.step(0);
}

TEST(LiveExecutionEngine, SkipsSymbolPricedAboveTradeNotional) {
  // trade_notional=$50 on stocks priced ~$100 yields qty=floor(50/100)=0.
  // Every symbol must be skipped; the broker must see zero limit orders.
  auto broker = std::make_unique<NiceMock<hft_test::MockIBroker>>();
  EXPECT_CALL(*broker, place_limit_order(_)).Times(0);

  hft::AppConfig app;
  app.mode = hft::BrokerMode::Paper;
  app.top_k = 5;
  app.steps = 1;
  app.trade_notional = 50.0;
  app.account_budget = 10000.0;
  app.max_open_symbols = 10;
  hft::LiveExecutionEngine engine(hft::LiveTradingConfig::from_app(app),
                                  std::move(broker));
  engine.initialize_universe(5);
  engine.step(0);
}

TEST(LiveExecutionEngine, SubscribeLiveBooksAlsoSubscribesTradesWhenEnabled) {
  auto broker = std::make_unique<NiceMock<hft_test::MockIBroker>>();
  // 3 symbols -> 3 top-of-book + 3 trade subscriptions.
  EXPECT_CALL(*broker, subscribe_top_of_book(_)).Times(3);
  EXPECT_CALL(*broker, subscribe_trades(_)).Times(3);
  hft::AppConfig app;
  app.mode = hft::BrokerMode::Paper;
  app.top_k = 3;
  app.hawkes_use_real_trades = true;
  hft::LiveExecutionEngine engine(hft::LiveTradingConfig::from_app(app),
                                  std::move(broker));
  engine.subscribe_live_books({"AAPL", "MSFT", "GOOG"});
}

TEST(LiveExecutionEngine, SubscribeLiveBooksSkipsTradesWhenDisabled) {
  auto broker = std::make_unique<NiceMock<hft_test::MockIBroker>>();
  EXPECT_CALL(*broker, subscribe_top_of_book(_)).Times(3);
  EXPECT_CALL(*broker, subscribe_trades(_)).Times(0);
  hft::AppConfig app;
  app.mode = hft::BrokerMode::Paper;
  app.top_k = 3;
  app.hawkes_use_real_trades = false;  // explicit
  hft::LiveExecutionEngine engine(hft::LiveTradingConfig::from_app(app),
                                  std::move(broker));
  engine.subscribe_live_books({"AAPL", "MSFT", "GOOG"});
}

TEST(LiveExecutionEngine, ReconcileDrivesHawkesFromRealTrades) {
  // With hawkes_use_real_trades=true, reconcile_broker_state must drain
  // trade events from the broker and feed them into Stock::hawkes. Each
  // event with event=1 lifts lambda by alpha=5 (decay over our chosen
  // dt is negligible), so after a single event lambda should jump well
  // above the default baseline of 10.
  auto broker = std::make_unique<NiceMock<hft_test::MockIBroker>>();
  // ticker_id 1 gets one trade; tickers 2..N get nothing.
  std::vector<hft::TradeEvent> ticker1_trades = {
      hft::TradeEvent{100.50, 100.0, 1'700'000'000'000'000'000LL}};
  ON_CALL(*broker, drain_trades(1))
      .WillByDefault(Return(ticker1_trades));
  ON_CALL(*broker, drain_trades(::testing::Ne(1)))
      .WillByDefault(Return(std::vector<hft::TradeEvent>{}));

  hft::AppConfig app;
  app.mode = hft::BrokerMode::Paper;
  app.top_k = 3;
  app.steps = 1;
  app.hawkes_use_real_trades = true;
  hft::LiveExecutionEngine engine(hft::LiveTradingConfig::from_app(app),
                                  std::move(broker));
  engine.initialize_universe(3);
  const double lambda_before = engine.ranking.portfolio.items[0].hawkes.lambda;
  engine.reconcile_broker_state();
  const double lambda_after = engine.ranking.portfolio.items[0].hawkes.lambda;
  EXPECT_GT(lambda_after, lambda_before);
  // Untouched symbols stay at baseline.
  EXPECT_DOUBLE_EQ(engine.ranking.portfolio.items[1].hawkes.lambda, 10.0);
}

TEST(LiveExecutionEngine, ReconcileSkipsHawkesUpdateWhenDisabled) {
  auto broker = std::make_unique<NiceMock<hft_test::MockIBroker>>();
  // drain_trades must NOT be called when the feature is off.
  EXPECT_CALL(*broker, drain_trades(::testing::_)).Times(0);
  hft::AppConfig app;
  app.mode = hft::BrokerMode::Paper;
  app.top_k = 3;
  app.steps = 1;
  app.hawkes_use_real_trades = false;
  hft::LiveExecutionEngine engine(hft::LiveTradingConfig::from_app(app),
                                  std::move(broker));
  engine.initialize_universe(3);
  engine.reconcile_broker_state();
}

TEST(LiveExecutionEngine, OUGateBlocksBuysAboveMean) {
  // ou.mu primed to 1.0 (far below the ranking engine's ~$100 default mids).
  // Every active candidate has mid > mu * (1 + threshold=0) -> every buy is
  // gated out.
  auto broker = std::make_unique<NiceMock<hft_test::MockIBroker>>();
  EXPECT_CALL(*broker, place_limit_order(_)).Times(0);

  hft::AppConfig app;
  app.mode = hft::BrokerMode::Paper;
  app.top_k = 5;
  app.steps = 1;
  app.ou_window_size = 100;
  app.ou_buy_threshold_pct = 0.0;
  hft::LiveExecutionEngine engine(hft::LiveTradingConfig::from_app(app),
                                  std::move(broker));
  engine.initialize_universe(5);
  for (auto& s : engine.ranking.portfolio.items) {
    s.ou.mu = 1.0;
    s.ou_initialized = true;
  }
  engine.step(0);
}

TEST(LiveExecutionEngine, OUGateAllowsBuysAtOrBelowMean) {
  // ou.mu primed well above mid: gate is permissive, ranking and budget
  // still let through at least one buy.
  auto broker = std::make_unique<NiceMock<hft_test::MockIBroker>>();
  EXPECT_CALL(*broker, place_limit_order(_)).Times(AtLeast(1));

  hft::AppConfig app;
  app.mode = hft::BrokerMode::Paper;
  app.top_k = 5;
  app.steps = 1;
  app.ou_window_size = 100;
  app.ou_buy_threshold_pct = 0.0;
  hft::LiveExecutionEngine engine(hft::LiveTradingConfig::from_app(app),
                                  std::move(broker));
  engine.initialize_universe(5);
  for (auto& s : engine.ranking.portfolio.items) {
    s.ou.mu = 1000.0;
    s.ou_initialized = true;
  }
  engine.step(0);
}

TEST(LiveExecutionEngine, OUGateDisabledWhenWindowSizeZero) {
  // ou_window_size=0 (default) disables the gate; even with ou.mu primed
  // far below mid the buy path must still place orders.
  auto broker = std::make_unique<NiceMock<hft_test::MockIBroker>>();
  EXPECT_CALL(*broker, place_limit_order(_)).Times(AtLeast(1));

  hft::AppConfig app;
  app.mode = hft::BrokerMode::Paper;
  app.top_k = 5;
  app.steps = 1;
  app.ou_window_size = 0;
  hft::LiveExecutionEngine engine(hft::LiveTradingConfig::from_app(app),
                                  std::move(broker));
  engine.initialize_universe(5);
  for (auto& s : engine.ranking.portfolio.items) {
    s.ou.mu = 1.0;            // would block if gate were active
    s.ou_initialized = true;
  }
  engine.step(0);
}

TEST(LiveExecutionEngine, StepHeartbeatBoundary) {
  // step(0) and step(100) both hit the (t % 100) == 0 heartbeat branch; step(1)
  // does not. We just assert the engine survives all three calls.
  auto broker = std::make_unique<NiceMock<hft_test::MockIBroker>>();
  EXPECT_CALL(*broker, place_limit_order(_)).Times(AtLeast(1));
  hft::LiveExecutionEngine engine(make_paper_config(), std::move(broker));
  engine.initialize_universe(5);
  engine.step(0);
  engine.step(1);
  engine.step(100);
}

}  // namespace
