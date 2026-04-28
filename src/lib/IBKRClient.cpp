#include "broker/IBKRClient.hpp"

#include <chrono>
#include <thread>

#include "log/logging_state.hpp"

namespace hft {

namespace hl = hft::log;

IBKRClient::~IBKRClient() {
  stop_event_loop();
  disconnect();
}

bool IBKRClient::connect(const std::string& host, int port, int client_id) {
  host_ = host;
  port_ = port;
  client_id_ = client_id;
  hl::set_component_state(hl::ComponentId::Broker,
                          hl::ComponentState::Starting);
#ifdef HFT_ENABLE_IBKR
  if (client_.eConnect(host.c_str(), port, client_id, false)) {
    connected_ = true;
    reader_ = new EReader(&client_, &signal_);
    reader_->start();
    hl::set_component_state(hl::ComponentId::Broker, hl::ComponentState::Ready);
    return true;
  }
  connected_ = false;
  hl::raise_error(hl::ComponentId::Broker, /*code=*/2, "IBKR eConnect failed");
  hl::set_component_state(hl::ComponentId::Broker, hl::ComponentState::Error,
                          /*code=*/2);
  return false;
#else
  (void)host;
  (void)port;
  (void)client_id;
  connected_ = true;
  hl::set_component_state(hl::ComponentId::Broker, hl::ComponentState::Ready);
  return true;
#endif
}

void IBKRClient::disconnect() {
#ifdef HFT_ENABLE_IBKR
  if (reader_ != nullptr) {
    reader_->stop();
    delete reader_;
    reader_ = nullptr;
  }
  if (client_.isConnected()) {
    client_.eDisconnect();
  }
#endif
  if (connected_) {
    hl::set_component_state(hl::ComponentId::Broker, hl::ComponentState::Down);
  }
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
  lifecycle_.on_submitted(req.id, req.symbol, req.qty);
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
  OrderCancel cancel;
  client_.cancelOrder(order_id, cancel);
#else
  (void)order_id;
#endif
}

double IBKRClient::ack_latency_ms(int order_id) const {
  const auto it = ack_latency_ms_cache_.find(order_id);
  if (it == ack_latency_ms_cache_.end()) {
    return 0.0;
  }
  return it->second;
}

void IBKRClient::start_event_loop() {
#ifdef HFT_ENABLE_IBKR
  if (reader_running_.exchange(true)) {
    return;
  }
  reader_thread_ = std::thread([this]() {
    while (reader_running_.load() && connected_) {
      signal_.waitForSignal();
      if (reader_ != nullptr) {
        reader_->processMsgs();
      }
    }
  });
#endif
}

void IBKRClient::stop_event_loop() {
#ifdef HFT_ENABLE_IBKR
  if (!reader_running_.exchange(false)) {
    return;
  }
  if (reader_thread_.joinable()) {
    reader_thread_.join();
  }
#endif
}

void IBKRClient::subscribe_market_depth(const MarketDepthRequest& req) {
#ifdef HFT_ENABLE_IBKR
  Contract contract;
  contract.symbol = req.symbol;
  contract.secType = "STK";
  contract.exchange = "SMART";
  contract.currency = "USD";
  client_.reqMktDepth(req.ticker_id, contract, req.depth, false,
                      TagValueListSPtr());
#else
  (void)req;
#endif
}

L2Book IBKRClient::snapshot_book(int ticker_id) const {
  std::lock_guard<std::mutex> lock(books_mutex_);
  const auto it = books_.find(ticker_id);
  if (it == books_.end()) {
    return {};
  }
  return it->second;
}

void IBKRClient::pump_once() {
#ifdef HFT_ENABLE_IBKR
  if (!connected_) {
    return;
  }
  signal_.waitForSignal();
  if (reader_ != nullptr) {
    reader_->processMsgs();
  }
#endif
}

bool IBKRClient::reconnect_once() {
  if (is_connected()) {
    reconnect_.reset();
    return true;
  }
  if (!reconnect_.should_retry()) {
    return false;
  }
  const int backoff = reconnect_.next_backoff_ms();
  std::this_thread::sleep_for(std::chrono::milliseconds(backoff));
  return connect(host_, port_, client_id_);
}

void IBKRClient::start_production_event_loop() {
#ifdef HFT_ENABLE_IBKR
  if (reader_running_.exchange(true)) {
    return;
  }
  reader_thread_ = std::thread([this]() {
    while (reader_running_.load()) {
      if (!is_connected()) {
        if (!reconnect_once()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(250));
          continue;
        }
      }
      pump_once();
    }
  });
#endif
}

#ifdef HFT_ENABLE_IBKR
void IBKRClient::connectionClosed() {
  const bool was_connected = connected_;
  connected_ = false;
  if (was_connected) {
    hl::raise_error(hl::ComponentId::Broker, /*code=*/3,
                    "IBKR connection closed by remote");
    hl::set_component_state(hl::ComponentId::Broker, hl::ComponentState::Error,
                            /*code=*/3);
  }
}

void IBKRClient::updateMktDepth(TickerId id, int position, int operation,
                                int side, double price, Decimal size) {
  std::lock_guard<std::mutex> lock(books_mutex_);
  auto& book = books_[id];
  if (position < 0 || position >= L2Book::DEPTH) {
    return;
  }
  const L2Level level{price, DecimalFunctions::decimalToDouble(size)};
  if (side == 0) {
    if (operation == 2) {
      book.bids[position] = {};
    } else {
      book.bids[position] = level;
    }
  } else {
    if (operation == 2) {
      book.asks[position] = {};
    } else {
      book.asks[position] = level;
    }
  }
}

void IBKRClient::orderStatus(OrderId orderId, const std::string& status,
                             Decimal filled, Decimal remaining,
                             double avgFillPrice, long long, int, double, int,
                             const std::string&, double) {
  lifecycle_.on_status(
      orderId, status, DecimalFunctions::decimalToDouble(filled),
      DecimalFunctions::decimalToDouble(remaining), avgFillPrice);
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
