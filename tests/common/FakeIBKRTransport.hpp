#pragma once

// Minimal hand-rolled IBKRTransport double for tests that exercise wiring
// around IBKRClient (engine lifecycle, broker-mode plumbing, etc.) without
// needing to dial the real TWS Gateway. connect() always succeeds and the
// transport reports itself as connected; everything else is a no-op.
//
// gmock-based MockIBKRTransport variants live in the new gtest-based unit
// suites; this header is here for the legacy HFT_TEST cases that just need
// "a working transport".

#include <string>

#include "broker/IBKRTransport.hpp"

namespace hft::test {

class FakeIBKRTransport final : public IBKRTransport {
 public:
  bool connect(const std::string& /*host*/, int /*port*/,
               int /*client_id*/) override {
    connected_ = true;
    return true;
  }

  void disconnect() override { connected_ = false; }

  [[nodiscard]] bool is_connected() const override { return connected_; }

  void place_limit_order(const OrderRequest& /*req*/) override {}
  void cancel_order(int /*order_id*/) override {}
  void subscribe_top_of_book(const TopOfBookRequest& /*req*/) override {}
  void subscribe_market_depth(const MarketDepthRequest& /*req*/) override {}
  void pump_once() override {}
  void set_callbacks(IBKRCallbacks* /*cb*/) override {}

 private:
  bool connected_ = false;
};

}  // namespace hft::test
