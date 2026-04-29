// Unit tests for hft::IBKRClient driven by a gmock IBKRTransport double.
//
// These tests cover the post-Phase-2 IBKRClient seam: every TWS API call goes
// through the injected IBKRTransport, so we can EXPECT_CALL on each delegation
// and exercise reader-thread lifecycle without a real broker socket. They
// also lock in the disconnect()-stops-reader-first ordering that Phase 2 added
// to fix a use-after-free on the EReader.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "broker/IBKRClient.hpp"
#include "broker/IBKRTransport.hpp"
#include "broker/IBroker.hpp"
#include "common/MockIBKRTransport.hpp"

namespace {

namespace hft_test = hft::test;

using ::testing::_;
using ::testing::AtLeast;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::StrictMock;

// Most tests don't care about set_callbacks() being called from the ctor or
// about pump_once() being called from a reader thread; NiceMock silences
// "uninteresting call" warnings for those.
using NiceMockTransport = NiceMock<hft_test::MockIBKRTransport>;

// Helper: install a transport whose connect/is_connected behaves like a real
// broker that just succeeded. Returns a raw pointer for EXPECT_CALL access
// (the IBKRClient owns the unique_ptr).
struct ClientWithMock {
  NiceMockTransport* mock{};
  hft::IBKRClient client;
  static ClientWithMock make() {
    auto t = std::make_unique<NiceMockTransport>();
    auto* raw = t.get();
    return ClientWithMock{raw, hft::IBKRClient(std::move(t))};
  }
};

TEST(IBKRClient, ConstructorSetsCallbacksOnTransport) {
  // The ctor must call transport_->set_callbacks(this) so inbound events are
  // routed to the IBKRCallbacks methods. The destructor will also call
  // is_connected()/disconnect() during cleanup; those are allowed.
  auto t = std::make_unique<StrictMock<hft_test::MockIBKRTransport>>();
  EXPECT_CALL(*t, set_callbacks(::testing::NotNull())).Times(1);
  EXPECT_CALL(*t, is_connected()).WillRepeatedly(Return(false));
  EXPECT_CALL(*t, disconnect()).Times(::testing::AnyNumber());
  hft::IBKRClient c(std::move(t));
}

TEST(IBKRClient, ConnectForwardsArgsAndReturnsResult) {
  auto t = std::make_unique<NiceMockTransport>();
  EXPECT_CALL(*t, connect(std::string("localhost"), 4002, 7))
      .WillOnce(Return(true));
  hft::IBKRClient c(std::move(t));
  EXPECT_TRUE(c.connect("localhost", 4002, 7));
}

TEST(IBKRClient, ConnectReturnsFalseOnTransportFailure) {
  auto t = std::make_unique<NiceMockTransport>();
  EXPECT_CALL(*t, connect(_, _, _)).WillOnce(Return(false));
  hft::IBKRClient c(std::move(t));
  EXPECT_FALSE(c.connect("h", 1, 2));
}

TEST(IBKRClient, IsConnectedDelegatesToTransport) {
  auto t = std::make_unique<NiceMockTransport>();
  // Explicit WillOnce sequence covers the two assertions; WillRepeatedly
  // absorbs the dtor's later is_connected() call.
  EXPECT_CALL(*t, is_connected())
      .WillOnce(Return(false))
      .WillOnce(Return(true))
      .WillRepeatedly(Return(false));
  hft::IBKRClient c(std::move(t));
  EXPECT_FALSE(c.is_connected());
  EXPECT_TRUE(c.is_connected());
}

TEST(IBKRClient, PlaceLimitOrderForwardsToTransport) {
  auto t = std::make_unique<NiceMockTransport>();
  EXPECT_CALL(*t, place_limit_order(_)).Times(1);
  hft::IBKRClient c(std::move(t));
  hft::OrderRequest req{};
  req.id = 11;
  req.symbol = "SYM";
  req.qty = 10.0;
  req.limit = 100.0;
  c.place_limit_order(req);
}

TEST(IBKRClient, CancelOrderForwardsToTransport) {
  auto t = std::make_unique<NiceMockTransport>();
  EXPECT_CALL(*t, cancel_order(99)).Times(1);
  hft::IBKRClient c(std::move(t));
  c.cancel_order(99);
}

TEST(IBKRClient, SubscribeMarketDepthForwardsToTransport) {
  auto t = std::make_unique<NiceMockTransport>();
  EXPECT_CALL(*t, subscribe_market_depth(_)).Times(1);
  hft::IBKRClient c(std::move(t));
  hft::MarketDepthRequest req{1, "FOO", 5};
  c.subscribe_market_depth(req);
}

TEST(IBKRClient, PumpOnceGatedOnIsConnected) {
  auto t = std::make_unique<NiceMockTransport>();
  // Disconnected: pump_once must NOT propagate. Dtor also calls is_connected
  // once; WillRepeatedly is the simpler way to express "always returns false".
  EXPECT_CALL(*t, is_connected()).WillRepeatedly(Return(false));
  EXPECT_CALL(*t, pump_once()).Times(0);
  hft::IBKRClient c(std::move(t));
  c.pump_once();
}

TEST(IBKRClient, PumpOnceForwardsWhenConnected) {
  auto t = std::make_unique<NiceMockTransport>();
  // First call (from c.pump_once) returns true; later dtor call returns false
  // so disconnect() doesn't go down the "was connected" branch.
  EXPECT_CALL(*t, is_connected())
      .WillOnce(Return(true))
      .WillRepeatedly(Return(false));
  EXPECT_CALL(*t, pump_once()).Times(1);
  hft::IBKRClient c(std::move(t));
  c.pump_once();
}

TEST(IBKRClient, DisconnectCallsTransportDisconnect) {
  auto t = std::make_unique<NiceMockTransport>();
  EXPECT_CALL(*t, disconnect()).Times(AtLeast(1));  // dtor may call again
  hft::IBKRClient c(std::move(t));
  c.disconnect();
}

TEST(IBKRClient, StartEventLoopIsIdempotent) {
  // Two consecutive start_event_loop() calls must produce exactly one running
  // reader thread; the second call is a no-op and stop_event_loop() must
  // still join cleanly.
  auto t = std::make_unique<NiceMockTransport>();
  // Reader checks transport_->is_connected() in its loop predicate. Return
  // false so the thread exits quickly without us needing to coordinate.
  EXPECT_CALL(*t, is_connected()).WillRepeatedly(Return(false));
  hft::IBKRClient c(std::move(t));
  c.start_event_loop();
  c.start_event_loop();  // second call - no-op
  c.stop_event_loop();
}

TEST(IBKRClient, StopEventLoopWithoutStartIsNoop) {
  auto t = std::make_unique<NiceMockTransport>();
  hft::IBKRClient c(std::move(t));
  c.stop_event_loop();  // must not deadlock or crash
  c.stop_event_loop();
}

TEST(IBKRClient, DisconnectStopsReaderThreadFirst) {
  // Phase-2 fix: disconnect() must call stop_event_loop() before tearing down
  // the transport so the reader thread is joined before transport-owned state
  // (EReader, sockets) becomes invalid.
  auto t = std::make_unique<NiceMockTransport>();
  std::atomic<int> in_pump{0};
  std::atomic<bool> let_pump_return{false};
  std::atomic<bool> disconnect_called{false};
  std::atomic<bool> disconnect_after_pump_returned{true};

  EXPECT_CALL(*t, is_connected()).WillRepeatedly(Return(true));
  EXPECT_CALL(*t, pump_once()).WillRepeatedly([&] {
    in_pump.fetch_add(1);
    while (!let_pump_return.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (disconnect_called.load()) {
      // disconnect() returned BEFORE pump_once() returned -> ordering bug.
      disconnect_after_pump_returned.store(false);
    }
  });
  EXPECT_CALL(*t, disconnect()).WillRepeatedly([&] {
    disconnect_called.store(true);
  });

  hft::IBKRClient c(std::move(t));
  c.start_event_loop();
  // Wait until reader thread is parked inside pump_once().
  while (in_pump.load() == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  // Allow pump_once to return after a short delay; in the meantime disconnect()
  // must wait for the reader thread to finish.
  std::thread releaser([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    let_pump_return.store(true);
  });
  c.disconnect();
  releaser.join();
  EXPECT_TRUE(disconnect_after_pump_returned.load())
      << "disconnect() returned before reader thread finished pump_once()";
}

TEST(IBKRClient, OnOrderStatusRecordsAckLatencyForSubmitted) {
  auto t = std::make_unique<NiceMockTransport>();
  hft::IBKRClient c(std::move(t));
  hft::OrderRequest req{};
  req.id = 5;
  req.symbol = "AA";
  req.qty = 1.0;
  c.place_limit_order(req);
  c.on_order_status(5, "Submitted", 0.0, 1.0, 0.0);
  EXPECT_GT(c.ack_latency_ms(5), 0.0);
}

TEST(IBKRClient, OnOrderStatusRecordsAckLatencyForPreSubmitted) {
  auto t = std::make_unique<NiceMockTransport>();
  hft::IBKRClient c(std::move(t));
  hft::OrderRequest req{};
  req.id = 6;
  req.symbol = "BB";
  req.qty = 1.0;
  c.place_limit_order(req);
  c.on_order_status(6, "PreSubmitted", 0.0, 1.0, 0.0);
  EXPECT_GT(c.ack_latency_ms(6), 0.0);
}

TEST(IBKRClient, OnOrderStatusUnrelatedStatusDoesNotRecordLatency) {
  auto t = std::make_unique<NiceMockTransport>();
  hft::IBKRClient c(std::move(t));
  hft::OrderRequest req{};
  req.id = 7;
  req.symbol = "CC";
  req.qty = 1.0;
  c.place_limit_order(req);
  c.on_order_status(7, "Filled", 1.0, 0.0, 100.0);
  // Ack latency only tracked for the initial Submitted/PreSubmitted ack.
  EXPECT_EQ(c.ack_latency_ms(7), 0.0);
}

TEST(IBKRClient, AckLatencyForUnknownOrderIsZero) {
  auto t = std::make_unique<NiceMockTransport>();
  hft::IBKRClient c(std::move(t));
  EXPECT_EQ(c.ack_latency_ms(12345), 0.0);
}

TEST(IBKRClient, OnMarketDepthUpdateBidSideInsert) {
  auto t = std::make_unique<NiceMockTransport>();
  hft::IBKRClient c(std::move(t));
  c.on_market_depth_update(/*ticker=*/1, /*pos=*/0, /*op=*/0, /*side=*/0, 100.0,
                           50.0);
  const auto book = c.snapshot_book(1);
  EXPECT_DOUBLE_EQ(book.bids[0].price, 100.0);
  EXPECT_DOUBLE_EQ(book.bids[0].size, 50.0);
}

TEST(IBKRClient, OnMarketDepthUpdateAskSideInsert) {
  auto t = std::make_unique<NiceMockTransport>();
  hft::IBKRClient c(std::move(t));
  c.on_market_depth_update(/*ticker=*/2, /*pos=*/1, /*op=*/0, /*side=*/1, 101.0,
                           25.0);
  const auto book = c.snapshot_book(2);
  EXPECT_DOUBLE_EQ(book.asks[1].price, 101.0);
  EXPECT_DOUBLE_EQ(book.asks[1].size, 25.0);
}

TEST(IBKRClient, OnMarketDepthUpdateDeleteOperationClearsLevel) {
  auto t = std::make_unique<NiceMockTransport>();
  hft::IBKRClient c(std::move(t));
  c.on_market_depth_update(1, 0, /*op=*/0, /*bid=*/0, 100.0, 50.0);
  // operation==2 means "delete this level"
  c.on_market_depth_update(1, 0, /*op=*/2, /*bid=*/0, 0.0, 0.0);
  const auto book = c.snapshot_book(1);
  EXPECT_DOUBLE_EQ(book.bids[0].price, 0.0);
  EXPECT_DOUBLE_EQ(book.bids[0].size, 0.0);
}

TEST(IBKRClient, OnMarketDepthUpdateOutOfRangePositionIgnored) {
  auto t = std::make_unique<NiceMockTransport>();
  hft::IBKRClient c(std::move(t));
  c.on_market_depth_update(1, /*pos=*/-1, 0, 0, 100.0, 50.0);   // negative
  c.on_market_depth_update(1, /*pos=*/999, 0, 0, 100.0, 50.0);  // too high
  const auto book = c.snapshot_book(1);
  EXPECT_DOUBLE_EQ(book.bids[0].price, 0.0);  // unchanged
}

TEST(IBKRClient, SnapshotBookForUnknownTickerIsEmpty) {
  auto t = std::make_unique<NiceMockTransport>();
  hft::IBKRClient c(std::move(t));
  const auto book = c.snapshot_book(/*unknown=*/424242);
  EXPECT_DOUBLE_EQ(book.best_bid(), 0.0);
  EXPECT_DOUBLE_EQ(book.best_ask(), 0.0);
}

TEST(IBKRClient, OnConnectionClosedDoesNotCrash) {
  auto t = std::make_unique<NiceMockTransport>();
  hft::IBKRClient c(std::move(t));
  c.on_connection_closed();  // emits state via logging API; must be safe.
}

TEST(IBKRClient, LifecycleAccessorReturnsBook) {
  auto t = std::make_unique<NiceMockTransport>();
  hft::IBKRClient c(std::move(t));
  hft::OrderRequest req{};
  req.id = 21;
  req.symbol = "XYZ";
  req.qty = 5.0;
  c.place_limit_order(req);
  EXPECT_TRUE(c.lifecycle().has(21));
}

TEST(IBKRClient, ReconnectOnceShortCircuitsWhenAlreadyConnected) {
  auto t = std::make_unique<NiceMockTransport>();
  EXPECT_CALL(*t, is_connected()).WillRepeatedly(Return(true));
  EXPECT_CALL(*t, connect(_, _, _)).Times(0);
  hft::IBKRClient c(std::move(t));
  EXPECT_TRUE(c.reconnect_once());
}

}  // namespace
