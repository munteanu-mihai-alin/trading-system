#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
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
  // Companion of ack_latency_ms_cache_ measuring placeOrder -> Filled.
  // Populated by on_order_status when status == "Filled"; under
  // event_mutex_ for the same races. Stays empty for orders that
  // never fill. Reset by stop_event_loop / disconnect lifecycle but
  // not on a per-order basis - tests / monitoring can read up to the
  // session boundary.
  std::unordered_map<int, double> fill_latency_ms_cache_;
  // Item 18: per-order realized commission. Populated by
  // on_commission_report; one order can receive multiple reports
  // (one per fill leg, identified by exec_id), so we accumulate.
  // Keyed by IBKR order_id. Under event_mutex_.
  std::unordered_map<int, double> commission_by_order_id_;
  std::unordered_map<int, TopOfBook> top_books_;
  std::unordered_map<int, L2Book> books_;
  // Per-ticker FIFO of trade prints, drained by the engine each step.
  std::unordered_map<int, std::vector<TradeEvent>> trade_events_;
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

  // Position-stream state. query_positions() blocks on positions_cv_ until
  // on_position_end() flips positions_stream_done_, then snapshots
  // pending_positions_ and cancels the stream. All three are guarded by
  // positions_mutex_.
  mutable std::mutex positions_mutex_;
  std::condition_variable positions_cv_;
  std::vector<BrokerPosition> pending_positions_;
  bool positions_stream_done_ = false;

  // Item 17: open-orders snapshot state. Same shape as positions
  // above. Filled by on_open_order callbacks; flushed when
  // on_open_order_end fires.
  mutable std::mutex open_orders_mutex_;
  std::condition_variable open_orders_cv_;
  std::vector<BrokerOpenOrder> pending_open_orders_;
  bool open_orders_stream_done_ = false;

  // ---- Subscription replay (audit #7) ----
  // Every subscribe_top_of_book / subscribe_market_depth /
  // subscribe_trades call appends to the matching vector. On
  // reconnect_once after a successful re-connect we walk these and
  // re-issue every entry so the engine doesn't run blind on stale
  // top_books_ / books_ after a Gateway flap. Tracked under
  // event_mutex_; reading + writing are both short.
  std::vector<TopOfBookRequest> subscribed_top_of_book_;
  std::vector<MarketDepthRequest> subscribed_depth_;
  std::vector<TopOfBookRequest> subscribed_trades_;

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
  void subscribe_top_of_book(const TopOfBookRequest& req) override;
  void subscribe_market_depth(const MarketDepthRequest& req) override;
  void subscribe_trades(const TopOfBookRequest& req) override;
  [[nodiscard]] std::vector<TradeEvent> drain_trades(int ticker_id) override;
  [[nodiscard]] std::vector<BrokerPosition> query_positions() override;
  [[nodiscard]] std::vector<BrokerOpenOrder> query_open_orders() override;
  void start_production_event_loop();
  void pump_once();
  bool reconnect_once();
  // Audit #7: replay every recorded subscription against the transport.
  // Called from reconnect_once after a successful reconnect AND
  // from the engine via the IBroker::reissue_subscriptions override
  // when error 1101/1102 lands. Also clears stale top_books_ /
  // books_ so the engine doesn't act on pre-flap data until the
  // new stream arrives.
  void reissue_subscriptions() override;

  [[nodiscard]] double ack_latency_ms(int order_id) const override;
  [[nodiscard]] double fill_latency_ms(int order_id) const override;
  [[nodiscard]] double realized_commission(int order_id) const override;
  [[nodiscard]] TopOfBook snapshot_top_of_book(int ticker_id) const override;
  [[nodiscard]] L2Book snapshot_book(int ticker_id) const override;
  [[nodiscard]] int next_valid_order_id() const;
  [[nodiscard]] std::vector<IBKRError> errors() const;
  // Audit #8: drain + clear the accumulated error buffer in one call.
  // Translates IBKRError -> IBroker::BrokerError so the engine can
  // react without depending on the IBKR-specific struct.
  [[nodiscard]] std::vector<BrokerError> drain_errors() override;
  [[nodiscard]] const OrderLifecycleBook* order_lifecycle() const override {
    return &lifecycle_;
  }
  [[nodiscard]] const OrderLifecycleBook& lifecycle() const {
    return lifecycle_;
  }

  // ---- IBKRCallbacks ----
  void on_order_status(int order_id, const std::string& status, double filled,
                       double remaining, double avg_fill_price) override;
  void on_market_depth_update(int ticker_id, int position, int operation,
                              int side, double price, double size) override;
  void on_top_of_book_price(int ticker_id, bool is_bid, double price) override;
  void on_top_of_book_size(int ticker_id, bool is_bid, double size) override;
  void on_trade(int ticker_id, double price, double qty,
                std::int64_t exch_ts_ns) override;
  void on_next_valid_id(int order_id) override;
  void on_error(const IBKRError& error) override;
  void on_position(const std::string& symbol, double qty,
                   double avg_cost) override;
  void on_position_end() override;
  void on_open_order(int order_id, const std::string& symbol,
                     const std::string& side, double qty,
                     double limit) override;
  void on_open_order_end() override;
  void on_commission_report(const std::string& exec_id, int order_id,
                            double commission_dollars) override;
  void on_connection_closed() override;
};

}  // namespace hft
