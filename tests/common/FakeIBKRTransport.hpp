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
#include <vector>

#include "broker/IBKRCallbacks.hpp"
#include "broker/IBKRTransport.hpp"

namespace hft::test {

// Holds one simulated position the fake should flush through on_position
// when the IBKRClient calls request_positions().
struct SimulatedPosition {
  std::string symbol;
  double qty = 0.0;
  double avg_cost = 0.0;
};

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
  void subscribe_trades(const TopOfBookRequest& /*req*/) override {}
  void pump_once() override {}
  void set_callbacks(IBKRCallbacks* cb) override { callbacks_ = cb; }

  // Seeds the positions the fake will deliver on the next
  // request_positions() call. Test helper; not part of the IBKRTransport
  // interface.
  void seed_positions(std::vector<SimulatedPosition> positions) {
    simulated_positions_ = std::move(positions);
  }

  void request_positions() override {
    if (callbacks_ == nullptr)
      return;
    // Flush synchronously: each position then position_end. The IBKRClient
    // wait_for predicate is satisfied before it ever has to block.
    for (const auto& p : simulated_positions_) {
      callbacks_->on_position(p.symbol, p.qty, p.avg_cost);
    }
    callbacks_->on_position_end();
  }

  void cancel_positions_stream() override { ++cancel_positions_count_; }

  // Item 17: open-orders flush via the same pattern.
  struct SimulatedOpenOrder {
    int order_id = 0;
    std::string symbol;
    std::string side;  // "buy" / "sell"
    double qty = 0.0;
    double limit = 0.0;
  };
  void seed_open_orders(std::vector<SimulatedOpenOrder> orders) {
    simulated_open_orders_ = std::move(orders);
  }
  void request_open_orders() override {
    if (callbacks_ == nullptr)
      return;
    for (const auto& o : simulated_open_orders_) {
      callbacks_->on_open_order(o.order_id, o.symbol, o.side, o.qty, o.limit);
    }
    callbacks_->on_open_order_end();
  }

  [[nodiscard]] int cancel_positions_count() const {
    return cancel_positions_count_;
  }

 private:
  bool connected_ = false;
  IBKRCallbacks* callbacks_ = nullptr;
  std::vector<SimulatedPosition> simulated_positions_;
  int cancel_positions_count_ = 0;
  std::vector<SimulatedOpenOrder> simulated_open_orders_;
};

}  // namespace hft::test
