#pragma once
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "broker/ConnectionSupervisor.hpp"
#include "broker/IBKRCallbacks.hpp"
#include "broker/IBKRTransport.hpp"
#include "broker/IBroker.hpp"
#include "broker/OrderLifecycle.hpp"
#include "models/l2_book.hpp"

namespace hft {

// Live broker driving real IBKR/TWS via an injected IBKRTransport. The class
// is free of TWS API headers and EWrapper inheritance; the TWS-specific code
// lives in RealIBKRTransport. Default-constructed instances pick up the
// production transport via make_default_ibkr_transport(); tests pass an
// explicit MockIBKRTransport.
class IBKRClient final : public IBroker, public IBKRCallbacks {
  std::unique_ptr<IBKRTransport> transport_;
  std::unordered_map<int, std::chrono::high_resolution_clock::time_point>
      send_ts_;
  std::unordered_map<int, double> ack_latency_ms_cache_;
  std::unordered_map<int, L2Book> books_;
  OrderLifecycleBook lifecycle_;
  std::vector<IBKRError> errors_;
  ConnectionSupervisor reconnect_;
  std::string host_;
  int port_ = 0;
  int client_id_ = 0;
  int next_valid_order_id_ = 0;

  mutable std::mutex books_mutex_;
  mutable std::mutex event_mutex_;
  std::atomic<bool> reader_running_{false};
  std::thread reader_thread_;

 public:
  IBKRClient();
  explicit IBKRClient(std::unique_ptr<IBKRTransport> transport);
  ~IBKRClient() override;

  bool connect(const std::string& host, int port, int client_id) override;
  void disconnect() override;
  bool is_connected() const override;
  void place_limit_order(const OrderRequest& req) override;
  void cancel_order(int order_id) override;
  void start_event_loop() override;
  void stop_event_loop() override;
  void subscribe_market_depth(const MarketDepthRequest& req) override;
  void start_production_event_loop();
  void pump_once();
  bool reconnect_once();

  [[nodiscard]] double ack_latency_ms(int order_id) const;
  [[nodiscard]] L2Book snapshot_book(int ticker_id) const;
  [[nodiscard]] int next_valid_order_id() const;
  [[nodiscard]] std::vector<IBKRError> errors() const;
  [[nodiscard]] const OrderLifecycleBook& lifecycle() const {
    return lifecycle_;
  }

  // ---- IBKRCallbacks ----
  void on_order_status(int order_id, const std::string& status, double filled,
                       double remaining, double avg_fill_price) override;
  void on_market_depth_update(int ticker_id, int position, int operation,
                              int side, double price, double size) override;
  void on_next_valid_id(int order_id) override;
  void on_error(const IBKRError& error) override;
  void on_connection_closed() override;
};

}  // namespace hft
