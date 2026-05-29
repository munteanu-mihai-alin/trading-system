#pragma once

#include <memory>
#include <string>

#include "broker/IBroker.hpp"

namespace hft {

class IBKRCallbacks;

// Outbound surface that IBKRClient uses to talk to the broker. The production
// implementation wraps the TWS API (EClientSocket + EReader + EWrapper); tests
// use a gmock to drive arbitrary scenarios.
//
// All TWS API knowledge (Decimal, Contract, Order, EWrapper overrides) lives
// behind this interface so IBKRClient itself stays free of TWS API headers.
class IBKRTransport {
 public:
  virtual ~IBKRTransport() = default;

  virtual bool connect(const std::string& host, int port, int client_id) = 0;
  virtual void disconnect() = 0;
  [[nodiscard]] virtual bool is_connected() const = 0;

  virtual void place_limit_order(const OrderRequest& req) = 0;
  virtual void cancel_order(int order_id) = 0;
  virtual void subscribe_top_of_book(const TopOfBookRequest& req) = 0;
  virtual void subscribe_market_depth(const MarketDepthRequest& req) = 0;
  // AllLast trade-print subscription. Default no-op so existing transport
  // doubles (FakeIBKRTransport, MockIBKRTransport) keep compiling without
  // change; production implementation lives in RealIBKRTransport and calls
  // EClientSocket::reqTickByTickData(..., "AllLast", ...).
  virtual void subscribe_trades(const TopOfBookRequest& /*req*/) {}

  // Begin streaming account positions. Each position fires through
  // IBKRCallbacks::on_position; the initial snapshot terminates with
  // IBKRCallbacks::on_position_end (after which IBKR keeps streaming live
  // updates until cancel_positions_stream). Default no-op so transport
  // doubles keep compiling; the real transport calls
  // EClientSocket::reqPositions().
  virtual void request_positions() {}
  // Stop streaming positions. Pairs with request_positions; safe to call
  // even if no stream is active. Default no-op.
  virtual void cancel_positions_stream() {}

  // Tell the broker which market-data tier to deliver. 1 = real-time only
  // (errors out if the subscription is missing rather than silently
  // delivering 15-minute-delayed prints). IBKRClient::connect calls this
  // with 1 right after a successful connect so the engine never operates
  // on stale data without an explicit alert from on_error (codes 354 /
  // 10167). Default no-op so transport doubles keep compiling.
  virtual void request_market_data_type(int /*data_type*/) {}
  // Ask the broker to push a fresh nextValidId via the on_next_valid_id
  // callback. Called from IBKRClient::connect to guarantee
  // next_valid_order_id_ matches the broker's view at session start, even
  // if a sibling client touched the account in between or the cached value
  // has drifted. The TWS API ignores the numIds parameter in modern
  // versions; pass -1 by convention. Default no-op.
  virtual void request_ids(int /*num_ids*/) {}

  // Block (with an internal timeout, typically ~2s) waiting for inbound
  // traffic from the broker, then dispatch one batch of decoded messages
  // through the registered IBKRCallbacks. Returns when at least one cycle
  // has been processed or the internal timeout elapses.
  virtual void pump_once() = 0;

  // Caller retains ownership of the callback sink; it must outlive the
  // transport. Set to nullptr to detach.
  virtual void set_callbacks(IBKRCallbacks* cb) = 0;
};

// Factory used by IBKRClient's default constructor. Defined in
// RealIBKRTransport.cpp.
std::unique_ptr<IBKRTransport> make_default_ibkr_transport();

}  // namespace hft
