#pragma once

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

  virtual void on_next_valid_id(int order_id) {}
  virtual void on_error(const IBKRError&) {}

  virtual void on_connection_closed() = 0;
};

}  // namespace hft
