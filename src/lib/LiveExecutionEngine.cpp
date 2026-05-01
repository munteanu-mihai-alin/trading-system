#include "engine/LiveExecutionEngine.hpp"
#include "broker/IBKRClient.hpp"
#include "log/logging_state.hpp"

#include <algorithm>
#include <utility>

namespace hft {

namespace hl = hft::log;

LiveExecutionEngine::LiveExecutionEngine(LiveTradingConfig cfg,
                                         std::unique_ptr<IBroker> broker)
    : cfg_(std::move(cfg)),
      broker_(std::move(broker)),
      ranking(cfg_.app.top_k, "shadow_results.csv") {}

bool LiveExecutionEngine::start() {
  hl::set_component_state(hl::ComponentId::Engine,
                          hl::ComponentState::Starting);
  if (cfg_.app.mode == BrokerMode::IBKRPaper &&
      !cfg_.app.allow_nonstandard_ibkr_paper_port && cfg_.app.port() != 4002 &&
      cfg_.app.port() != 7497) {
    hl::raise_error(hl::ComponentId::Engine, /*code=*/4,
                    "ibkr_paper mode requires paper Gateway/TWS port");
    hl::set_component_state(hl::ComponentId::Engine, hl::ComponentState::Error,
                            /*code=*/4);
    return false;
  }

  const bool ok =
      broker_->connect(cfg_.app.host, cfg_.app.port(), cfg_.app.client_id);
  if (!ok) {
    hl::raise_error(hl::ComponentId::Engine, /*code=*/1,
                    "broker connect failed");
    hl::set_component_state(hl::ComponentId::Engine, hl::ComponentState::Error,
                            /*code=*/1);
    return false;
  }
  hl::set_component_state(hl::ComponentId::Engine, hl::ComponentState::Ready);
  return true;
}

void LiveExecutionEngine::stop() {
  broker_->disconnect();
  hl::set_component_state(hl::ComponentId::Engine, hl::ComponentState::Down);
}

void LiveExecutionEngine::initialize_universe(int n_stocks) {
  hl::set_component_state(hl::ComponentId::Universe,
                          hl::ComponentState::Starting);
  ranking.initialize(n_stocks);
  hl::set_component_state(hl::ComponentId::Universe, hl::ComponentState::Ready);
}

void LiveExecutionEngine::step(int t) {
  reconcile_broker_state();
  ranking.step(t);

  if (!cfg_.app.order_enabled) {
    if ((t % 100) == 0) {
      hl::heartbeat(hl::ComponentId::Engine);
    }
    return;
  }

  for (const auto& s : ranking.portfolio.items) {
    if (!s.active)
      continue;
    if (!can_route_order(s))
      continue;

    OrderRequest req;
    req.id = next_order_id_++;
    req.symbol = s.symbol;
    req.is_buy = true;
    req.qty = cfg_.app.order_qty;
    if (cfg_.app.max_order_qty > 0.0) {
      req.qty = std::min(req.qty, cfg_.app.max_order_qty);
    }
    req.limit = s.best_limit;
    if (req.qty <= 0.0 || req.limit <= 0.0)
      continue;
    if (cfg_.app.max_notional_per_order > 0.0 &&
        req.qty * req.limit > cfg_.app.max_notional_per_order) {
      continue;
    }
    broker_->place_limit_order(req);
    ++orders_placed_;
    ++symbol_order_counts_[s.symbol];
  }

  // Heartbeat the engine roughly every 100 steps so the registry's
  // last_update_ns advances without flooding the log with one event per step.
  if ((t % 100) == 0) {
    hl::heartbeat(hl::ComponentId::Engine);
  }
}

bool LiveExecutionEngine::can_route_order(const Stock& stock) const {
  if (cfg_.app.max_orders_per_run > 0 &&
      orders_placed_ >= cfg_.app.max_orders_per_run) {
    return false;
  }
  if (cfg_.app.max_orders_per_symbol > 0) {
    const auto it = symbol_order_counts_.find(stock.symbol);
    const int count = (it == symbol_order_counts_.end()) ? 0 : it->second;
    if (count >= cfg_.app.max_orders_per_symbol) {
      return false;
    }
  }
  return true;
}

}  // namespace hft

namespace hft {

void LiveExecutionEngine::subscribe_live_books(
    const std::vector<std::string>& symbols) {
  hl::set_component_state(hl::ComponentId::MarketData,
                          hl::ComponentState::Starting);
  int ticker = 1;
  for (const auto& sym : symbols) {
    broker_->subscribe_market_depth(MarketDepthRequest{ticker++, sym, 5});
  }
  hl::set_component_state(hl::ComponentId::MarketData,
                          hl::ComponentState::Ready);
}

void LiveExecutionEngine::reconcile_broker_state() {
  auto* ibkr = dynamic_cast<IBKRClient*>(broker_.get());
  if (ibkr == nullptr)
    return;

  for (std::size_t i = 0; i < ranking.portfolio.items.size(); ++i) {
    auto& s = ranking.portfolio.items[i];
    const auto book = ibkr->snapshot_book(static_cast<int>(i + 1));
    if (book.best_bid() > 0.0 && book.best_ask() > 0.0) {
      s.mid = 0.5 * (book.best_bid() + book.best_ask());
      if (book.bids[0].size > 0.0) {
        s.queue = book.bids[0].size;
      }
    }
  }
  ranking.portfolio.rank();
}

}  // namespace hft
