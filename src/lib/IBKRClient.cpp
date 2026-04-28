#include "broker/IBKRClient.hpp"

#include <chrono>
#include <thread>
#include <utility>

#include "log/logging_state.hpp"

namespace hft {

namespace hl = hft::log;

IBKRClient::IBKRClient() : IBKRClient(make_default_ibkr_transport()) {}

IBKRClient::IBKRClient(std::unique_ptr<IBKRTransport> transport)
    : transport_(std::move(transport)) {
  transport_->set_callbacks(this);
}

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
  if (transport_->connect(host, port, client_id)) {
    hl::set_component_state(hl::ComponentId::Broker, hl::ComponentState::Ready);
    return true;
  }
  hl::raise_error(hl::ComponentId::Broker, /*code=*/2,
                  "IBKR transport connect failed");
  hl::set_component_state(hl::ComponentId::Broker, hl::ComponentState::Error,
                          /*code=*/2);
  return false;
}

void IBKRClient::disconnect() {
  // The reader/production thread may be blocked inside the transport's
  // pump_once() when this is called from arbitrary user code (the destructor
  // already orders the stop correctly, but explicit disconnect() needs the
  // same guarantee). Joining first prevents a use-after-free on transport-
  // owned state (EReader, sockets) once the transport is torn down below.
  stop_event_loop();
  const bool was_connected = transport_->is_connected();
  transport_->disconnect();
  if (was_connected) {
    hl::set_component_state(hl::ComponentId::Broker, hl::ComponentState::Down);
  }
}

bool IBKRClient::is_connected() const {
  return transport_->is_connected();
}

void IBKRClient::place_limit_order(const OrderRequest& req) {
  lifecycle_.on_submitted(req.id, req.symbol, req.qty);
  send_ts_[req.id] = std::chrono::high_resolution_clock::now();
  transport_->place_limit_order(req);
}

void IBKRClient::cancel_order(int order_id) {
  transport_->cancel_order(order_id);
}

double IBKRClient::ack_latency_ms(int order_id) const {
  const auto it = ack_latency_ms_cache_.find(order_id);
  if (it == ack_latency_ms_cache_.end()) {
    return 0.0;
  }
  return it->second;
}

void IBKRClient::start_event_loop() {
  if (reader_running_.exchange(true)) {
    return;
  }
  reader_thread_ = std::thread([this]() {
    while (reader_running_.load() && transport_->is_connected()) {
      transport_->pump_once();
    }
  });
}

void IBKRClient::stop_event_loop() {
  if (!reader_running_.exchange(false)) {
    return;
  }
  if (reader_thread_.joinable()) {
    reader_thread_.join();
  }
}

void IBKRClient::subscribe_market_depth(const MarketDepthRequest& req) {
  transport_->subscribe_market_depth(req);
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
  if (!transport_->is_connected()) {
    return;
  }
  transport_->pump_once();
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
      transport_->pump_once();
    }
  });
}

// ---- IBKRCallbacks ----

void IBKRClient::on_order_status(int order_id, const std::string& status,
                                 double filled, double remaining,
                                 double avg_fill_price) {
  lifecycle_.on_status(order_id, status, filled, remaining, avg_fill_price);
  if (status == "Submitted" || status == "PreSubmitted") {
    const auto it = send_ts_.find(order_id);
    if (it != send_ts_.end()) {
      const auto now = std::chrono::high_resolution_clock::now();
      const double ms =
          std::chrono::duration<double, std::milli>(now - it->second).count();
      ack_latency_ms_cache_[order_id] = ms;
    }
  }
}

void IBKRClient::on_market_depth_update(int ticker_id, int position,
                                        int operation, int side, double price,
                                        double size) {
  std::lock_guard<std::mutex> lock(books_mutex_);
  auto& book = books_[ticker_id];
  if (position < 0 || position >= L2Book::DEPTH) {
    return;
  }
  const L2Level level{price, size};
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

void IBKRClient::on_connection_closed() {
  hl::raise_error(hl::ComponentId::Broker, /*code=*/3,
                  "IBKR connection closed by remote");
  hl::set_component_state(hl::ComponentId::Broker, hl::ComponentState::Error,
                          /*code=*/3);
}

}  // namespace hft
