#pragma once

#include <cstdint>
#include <string>

namespace hft {

struct IBKRError {
  int request_id = 0;
  int code = 0;
  std::string message;
  std::string advanced_order_reject_json;
};

// Inbound surface for the IBKRTransport: the callback sink that receives
// already-decoded events. The real transport translates EWrapper callbacks
// (with TWS-specific types like Decimal) into these portable C++ types so
// IBKRClient never sees the TWS API surface.
//
// All methods must be safe to call from the transport's reader thread.
class IBKRCallbacks {
 public:
  virtual ~IBKRCallbacks() = default;

  virtual void on_order_status(int order_id, const std::string& status,
                               double filled, double remaining,
                               double avg_fill_price) = 0;

  virtual void on_market_depth_update(int ticker_id, int position,
                                      int operation, int side, double price,
                                      double size) = 0;

  virtual void on_top_of_book_price(int ticker_id, bool is_bid, double price) {}
  virtual void on_top_of_book_size(int ticker_id, bool is_bid, double size) {}

  // Single trade print on `ticker_id`. `exch_ts_ns` is the venue timestamp
  // in Unix nanoseconds; consumers compute event spacing from it to drive
  // Hawkes (or any other event-rate model). Default no-op so callbacks
  // that don't care about trades don't have to override.
  virtual void on_trade(int ticker_id, double price, double qty,
                        std::int64_t exch_ts_ns) {}

  virtual void on_next_valid_id(int order_id) {}
  virtual void on_error(const IBKRError&) {}

  // One position from a reqPositions stream. Symbol comes from the TWS
  // Contract.symbol; qty is signed (negative for shorts); avg_cost is the
  // broker's average cost (cost basis / qty), already including entry-side
  // commissions. Default no-op so callbacks that don't care about positions
  // don't have to override.
  virtual void on_position(const std::string& /*symbol*/, double /*qty*/,
                           double /*avg_cost*/) {}
  // Signal that reqPositions has delivered all currently-known positions.
  // After this fires the broker will keep streaming live updates until
  // cancelPositions is called.
  virtual void on_position_end() {}

  // Item 17: one open order from a reqAllOpenOrders stream. side is
  // "buy" or "sell" matching the orders.csv schema. Followed by an
  // on_open_order_end() once the broker has delivered every order.
  virtual void on_open_order(int /*order_id*/, const std::string& /*symbol*/,
                             const std::string& /*side*/, double /*qty*/,
                             double /*limit*/) {}
  virtual void on_open_order_end() {}

  // Item 18: commissionAndFeesReport. IBKR emits one of these per fill
  // (sometimes split across multiple events for one order via execId).
  // Dollars, post-trade. The exec_id lets the engine accumulate per
  // logical order even when the broker splits the report. Default
  // no-op so test doubles that don't care don't have to override.
  virtual void on_commission_report(const std::string& /*exec_id*/,
                                    int /*order_id*/,
                                    double /*commission_dollars*/) {}

  virtual void on_connection_closed() = 0;
};

}  // namespace hft
