#pragma once
#include <string>

namespace hft {

struct MarketDepthRequest {
    int ticker_id = 0;
    std::string symbol;
    int depth = 5;
};

struct OrderRequest {
    int id = 0;
    std::string symbol;
    bool is_buy = true;
    double qty = 0.0;
    double limit = 0.0;
};

struct OrderUpdate {
    int id = 0;
    std::string status;
    double filled = 0.0;
    double remaining = 0.0;
    double avg_fill_price = 0.0;
};

class IBroker {
   public:
    virtual ~IBroker() = default;
    virtual bool connect(const std::string& host, int port, int client_id) = 0;
    virtual void disconnect() = 0;
    virtual bool is_connected() const = 0;
    virtual void place_limit_order(const OrderRequest& req) = 0;
    virtual void cancel_order(int order_id) = 0;
    virtual void start_event_loop() = 0;
    virtual void stop_event_loop() = 0;
    virtual void subscribe_market_depth(const MarketDepthRequest& req) = 0;
};

}  // namespace hft
