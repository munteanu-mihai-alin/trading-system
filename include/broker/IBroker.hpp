#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "broker/OrderLifecycle.hpp"
#include "models/l2_book.hpp"

namespace hft {

struct TradeEvent {
  double price = 0.0;
  double qty = 0.0;
  std::int64_t exch_ts_ns = 0;
};

struct MarketDepthRequest {
  int ticker_id = 0;
  std::string symbol;
  int depth = 5;
  // Optional disambiguation for SMART routing. Empty means "let IBKR pick"
  // (works for most US single-listed stocks). For dual-listed or ambiguous
  // symbols (PSTG was the first one we hit on the L1 backfill), set this
  // to the listing exchange ("NASDAQ", "NYSE", "ARCA", ...) so
  // reqContractDetails resolves to exactly one contract. See
  // agent/ibkr_client_audit.md #1 and #9.
  std::string primary_exchange;
};

struct TopOfBookRequest {
  int ticker_id = 0;
  std::string symbol;
  // See MarketDepthRequest::primary_exchange.
  std::string primary_exchange;
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
  // Optional, same semantics as MarketDepthRequest::primary_exchange. When
  // set, RealIBKRTransport pins Contract.primaryExchange so the order
  // routes to the exact listing the engine subscribed against. Leaving it
  // empty preserves today's SMART-only behaviour.
  std::string primary_exchange;
};

struct OrderUpdate {
  int id = 0;
  std::string status;
  double filled = 0.0;
  double remaining = 0.0;
  double avg_fill_price = 0.0;
};

// One open position held in the broker's account state. Returned by
// IBroker::query_positions() at engine startup so the engine can recover
// open_positions_ across restarts rather than starting from an empty map.
//
// avg_cost is IBKR's "average cost" semantics: total cost basis / qty,
// already including commissions on entry. The engine treats it as the
// entry price for sell-target calculations.
struct BrokerPosition {
  std::string symbol;
  double qty = 0.0;
  double avg_cost = 0.0;
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
  // Subscribe to AllLast trade prints on ticker_id. Default no-op so brokers
  // that don't deliver trades (DatabentoBacktestBroker, LocalSimBroker) keep
  // compiling. Live IBKR implements it via reqTickByTickData.
  virtual void subscribe_trades(const TopOfBookRequest& /*req*/) {}
  // Drain (and clear) the per-ticker trade events accumulated since the last
  // call. Returns empty for brokers that don't deliver trades. Engine polls
  // this each step to drive Hawkes from real market activity.
  [[nodiscard]] virtual std::vector<TradeEvent> drain_trades(
      int /*ticker_id*/) {
    return {};
  }

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

  // Backtest brokers that load finite replay data can report the maximum
  // step index they have data for. main.cpp uses this to cap cfg.steps
  // when AppConfig::steps_auto_from_broker is set, avoiding the common
  // pitfall of running far past the data window's end (where L1/L2
  // freeze and the strategy is making decisions on stale prices).
  // Default 0 means "no limit reported"; live brokers should keep this.
  [[nodiscard]] virtual int max_replay_steps() const { return 0; }

  // Snapshot of positions currently held in the broker's account. Used by
  // LiveExecutionEngine::start() to rebuild open_positions_ across
  // restarts so an existing IBKR position isn't orphaned (engine wouldn't
  // try to sell it, wouldn't honor the budget gate against it, and
  // max_open_symbols accounting would be wrong).
  //
  // Default returns empty - backtest brokers don't carry forward state
  // across runs, and unwired live brokers degrade to today's behaviour
  // (fresh empty positions map) rather than crashing.
  [[nodiscard]] virtual std::vector<BrokerPosition> query_positions() {
    return {};
  }
};

}  // namespace hft
