#pragma once
#include <chrono>
#include <string>
#include <unordered_map>

#include "broker/IBroker.hpp"

namespace hft {

// Compile-safe IBKR adapter skeleton.
// Define HFT_ENABLE_IBKR and add official IBKR headers/libs to wire it for real.
// This header preserves the modular architecture without breaking builds.
class IBKRClient : public IBroker {
    bool connected_ = false;
    std::unordered_map<int, std::chrono::high_resolution_clock::time_point> send_ts_;

public:
    bool connect(const std::string& host, int port, int client_id) override;
    void disconnect() override;
    bool is_connected() const override;
    void place_limit_order(const OrderRequest& req) override;
    void cancel_order(int order_id) override;

    [[nodiscard]] double ack_latency_ms(int order_id) const;
};

}  // namespace hft
