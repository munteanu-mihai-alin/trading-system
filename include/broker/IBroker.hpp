#pragma once
#include <string>

#include "broker/OrderLifecycle.hpp"
#include "models/l2_book.hpp"

namespace hft {

struct MarketDepthRequest {
  int ticker_id = 0;
  std::string symbol;
  int depth = 5;
};

struct TopOfBookRequest {
  int ticker_id = 0;
  std::string symbol;
};

struct TopOfBook {
  double bid_price = 0.0;
  double bid_size = 0.0;
  double ask_price = 0.0;
  double ask_size = 0.0;

  [[nodiscard]] bool valid() const {
    return bid_price > 0.0 && ask_price > 0.0 && bid_price <= ask_price;
  }

  [[nodiscard]] double mid() const {
    if (!valid())
      return 0.0;
    return 0.5 * (bid_price + ask_price);
  }
};

struct OrderRequest {
  int id = 0;
  std::string symbol;
  bool is_buy = true;
  double qty = 0.0;
  double limit = 0.0;
  bool transmit = true;
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
  virtual void subscribe_top_of_book(const TopOfBookRequest& /*req*/) {}
  virtual void subscribe_market_depth(const MarketDepthRequest& req) = 0;

  virtual void on_step(int /*t*/) {}

  [[nodiscard]] virtual L2Book snapshot_book(int /*ticker_id*/) const {
    return {};
  }

  [[nodiscard]] virtual TopOfBook snapshot_top_of_book(
      int /*ticker_id*/) const {
    return {};
  }

  [[nodiscard]] virtual const OrderLifecycleBook* order_lifecycle() const {
    return nullptr;
  }

  [[nodiscard]] virtual double ack_latency_ms(int /*order_id*/) const {
    return 0.0;
  }
};

}  // namespace hft
