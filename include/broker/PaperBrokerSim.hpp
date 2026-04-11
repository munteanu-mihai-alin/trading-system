#pragma once
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "broker/IBroker.hpp"

namespace hft {

// Compile-safe broker used for paper/sim mode and tests.
// It records placed/cancelled orders and can emit synthetic updates.
class PaperBrokerSim : public IBroker {
    bool connected_ = false;

public:
    std::vector<OrderRequest> placed;
    std::vector<int> cancelled;
    std::deque<OrderUpdate> updates;

    bool connect(const std::string&, int, int) override {
        connected_ = true;
        return true;
    }

    void disconnect() override {
        connected_ = false;
    }

    bool is_connected() const override {
        return connected_;
    }

    void place_limit_order(const OrderRequest& req) override {
        placed.push_back(req);
        updates.push_back(OrderUpdate{req.id, "Submitted", 0.0, req.qty, 0.0});
    }

    void cancel_order(int order_id) override {
        cancelled.push_back(order_id);
        updates.push_back(OrderUpdate{order_id, "Cancelled", 0.0, 0.0, 0.0});
    }

    bool poll_update(OrderUpdate& out) {
        if (updates.empty()) return false;
        out = updates.front();
        updates.pop_front();
        return true;
    }
};

}  // namespace hft
