#pragma once

// gmock-based double for IBroker. Used by gtest-based unit tests for
// LiveExecutionEngine and other components that own an IBroker. Lets tests
// EXPECT_CALL on connect/disconnect/place/cancel without going through the
// IBKRClient -> IBKRTransport stack.

#include <gmock/gmock.h>

#include <string>

#include "broker/IBroker.hpp"

namespace hft::test {

class MockIBroker : public IBroker {
 public:
  MOCK_METHOD(bool, connect, (const std::string& host, int port, int client_id),
              (override));
  MOCK_METHOD(void, disconnect, (), (override));
  MOCK_METHOD(bool, is_connected, (), (const, override));
  MOCK_METHOD(void, place_limit_order, (const OrderRequest& req), (override));
  MOCK_METHOD(void, cancel_order, (int order_id), (override));
  MOCK_METHOD(void, start_event_loop, (), (override));
  MOCK_METHOD(void, stop_event_loop, (), (override));
  MOCK_METHOD(void, subscribe_market_depth, (const MarketDepthRequest& req),
              (override));
};

}  // namespace hft::test
