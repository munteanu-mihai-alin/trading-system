
#include "broker/IBKRClient.hpp"

#ifdef HFT_ENABLE_IBKR
#include <thread>
#endif

namespace hft {

IBKRClient::~IBKRClient() {
    disconnect();
}

bool IBKRClient::connect(const std::string& host, int port, int client_id) {
#ifdef HFT_ENABLE_IBKR
    if (client_.eConnect(host.c_str(), port, client_id, false)) {
        connected_ = true;
        reader_ = new EReader(&client_, &signal_);
        reader_->start();
        return true;
    }
    connected_ = false;
    return false;
#else
    (void)host; (void)port; (void)client_id;
    connected_ = true;
    return true;
#endif
}

void IBKRClient::disconnect() {
#ifdef HFT_ENABLE_IBKR
    if (reader_ != nullptr) {
        delete reader_;
        reader_ = nullptr;
    }
    client_.eDisconnect();
#endif
    connected_ = false;
}

bool IBKRClient::is_connected() const {
#ifdef HFT_ENABLE_IBKR
    return connected_ && client_.isConnected();
#else
    return connected_;
#endif
}

void IBKRClient::place_limit_order(const OrderRequest& req) {
    send_ts_[req.id] = std::chrono::high_resolution_clock::now();
#ifdef HFT_ENABLE_IBKR
    Contract contract;
    contract.symbol = req.symbol;
    contract.secType = "STK";
    contract.exchange = "SMART";
    contract.currency = "USD";

    Order order;
    order.orderId = req.id;
    order.action = req.is_buy ? "BUY" : "SELL";
    order.orderType = "LMT";
    order.totalQuantity = DecimalFunctions::doubleToDecimal(req.qty);
    order.lmtPrice = req.limit;

    client_.placeOrder(req.id, contract, order);
#else
    (void)req;
#endif
}

void IBKRClient::cancel_order(int order_id) {
#ifdef HFT_ENABLE_IBKR
    client_.cancelOrder(order_id, "");
#else
    (void)order_id;
#endif
}

double IBKRClient::ack_latency_ms(int order_id) const {
    const auto it = ack_latency_ms_cache_.find(order_id);
    if (it == ack_latency_ms_cache_.end()) return 0.0;
    return it->second;
}


void IBKRClient::start_event_loop() {
#ifdef HFT_ENABLE_IBKR
    if (reader_running_.exchange(true)) return;

    reader_thread_ = std::thread([this]() {
        while (reader_running_.load() && connected_) {
            signal_.waitForSignal();
            if (reader_) {
                reader_->processMsgs();
            }
        }
    });
#endif
}

void IBKRClient::stop_event_loop() {
#ifdef HFT_ENABLE_IBKR
    if (!reader_running_.exchange(false)) return;
    if (reader_thread_.joinable()) reader_thread_.join();
#endif
}

void IBKRClient::subscribe_market_depth(const MarketDepthRequest& req) {
#ifdef HFT_ENABLE_IBKR
    Contract contract;
    contract.symbol = req.symbol;
    contract.secType = "STK";
    contract.exchange = "SMART";
    contract.currency = "USD";
    client_.reqMktDepth(req.ticker_id, contract, req.depth, false, {});
#else
    (void)req;
#endif
}

#ifdef HFT_ENABLE_IBKR
void IBKRClient::orderStatus(OrderId orderId,
                             const std::string& status,
                             Decimal,
                             Decimal,
                             double,
                             int,
                             int,
                             double,
                             int,
                             const std::string&,
                             double) {
    if (status == "Submitted" || status == "PreSubmitted") {
        const auto it = send_ts_.find(orderId);
        if (it != send_ts_.end()) {
            const auto now = std::chrono::high_resolution_clock::now();
            const double ms =
                std::chrono::duration<double, std::milli>(now - it->second).count();
            ack_latency_ms_cache_[orderId] = ms;
        }
    }
}
#endif

}  // namespace hft
