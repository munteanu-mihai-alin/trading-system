#pragma once

// gmock-based double for IBKRTransport. Used by gtest-based unit tests under
// tests/unit/ to drive IBKRClient through arbitrary scenarios without a real
// broker socket.
//
// The hand-rolled FakeIBKRTransport in this same directory is kept for the
// legacy HFT_TEST cases under tests/module/ that just need "a working
// transport"; new unit tests should prefer this mock so they can EXPECT_CALL
// on individual methods.

#include <gmock/gmock.h>

#include <string>

#include "broker/IBKRCallbacks.hpp"
#include "broker/IBKRTransport.hpp"
#include "broker/IBroker.hpp"

namespace hft::test {

class MockIBKRTransport : public IBKRTransport {
 public:
  MOCK_METHOD(bool, connect,
              (const std::string& host, int port, int client_id),
              (override));
  MOCK_METHOD(void, disconnect, (), (override));
  MOCK_METHOD(bool, is_connected, (), (const, override));
  MOCK_METHOD(void, place_limit_order, (const OrderRequest& req), (override));
  MOCK_METHOD(void, cancel_order, (int order_id), (override));
  MOCK_METHOD(void, subscribe_market_depth, (const MarketDepthRequest& req),
              (override));
  MOCK_METHOD(void, pump_once, (), (override));
  MOCK_METHOD(void, set_callbacks, (IBKRCallbacks * cb), (override));
};

}  // namespace hft::test
