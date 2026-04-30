#pragma once
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "broker/IBroker.hpp"
#include "log/logging_state.hpp"

namespace hft {

// Local in-memory broker used for simulation and unit tests. This is not an
// IBKR paper account connection; use IBKRClient against a paper Gateway/TWS
// socket for orders that should appear in IBKR paper trading.
class LocalSimBroker : public IBroker {
  bool connected_ = false;

 public:
  std::vector<OrderRequest> placed;
  std::vector<int> cancelled;
  std::deque<OrderUpdate> updates;

  bool connect(const std::string&, int, int) override {
    connected_ = true;
    hft::log::set_component_state(hft::log::ComponentId::Broker,
                                  hft::log::ComponentState::Ready);
    return true;
  }

  void disconnect() override {
    if (connected_) {
      hft::log::set_component_state(hft::log::ComponentId::Broker,
                                    hft::log::ComponentState::Down);
    }
    connected_ = false;
  }

  bool is_connected() const override { return connected_; }

  void place_limit_order(const OrderRequest& req) override {
    placed.push_back(req);
    updates.push_back(OrderUpdate{req.id, "Submitted", 0.0, req.qty, 0.0});
  }

  void cancel_order(int order_id) override {
    cancelled.push_back(order_id);
    updates.push_back(OrderUpdate{order_id, "Cancelled", 0.0, 0.0, 0.0});
  }

  void start_event_loop() override {}
  void stop_event_loop() override {}
  void subscribe_market_depth(const MarketDepthRequest&) override {}

  bool poll_update(OrderUpdate& out) {
    if (updates.empty())
      return false;
    out = updates.front();
    updates.pop_front();
    return true;
  }
};

}  // namespace hft
