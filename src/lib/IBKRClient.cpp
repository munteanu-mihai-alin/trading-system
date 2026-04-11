#include "broker/IBKRClient.hpp"

namespace hft {

bool IBKRClient::connect(const std::string&, int, int) {
    connected_ = true;
    return true;
}

void IBKRClient::disconnect() {
    connected_ = false;
}

bool IBKRClient::is_connected() const {
    return connected_;
}

void IBKRClient::place_limit_order(const OrderRequest& req) {
    send_ts_[req.id] = std::chrono::high_resolution_clock::now();
}

void IBKRClient::cancel_order(int) {
}

double IBKRClient::ack_latency_ms(int order_id) const {
    const auto it = send_ts_.find(order_id);
    if (it == send_ts_.end()) return 0.0;
    const auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double, std::milli>(now - it->second).count();
}

}  // namespace hft
