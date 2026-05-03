#include "engine/LiveExecutionEngine.hpp"
#include "broker/IBKRClient.hpp"
#include "execution/InstitutionalTransactionCostModel.h"
#include "execution/score.hpp"
#include "log/logging_state.hpp"
#include "models/l2_book.hpp"
#include "models/micro.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace hft {

namespace hl = hft::log;

namespace {

[[nodiscard]] bool is_terminal_order_status(OrderLifecycleStatus status) {
  return status == OrderLifecycleStatus::Filled ||
         status == OrderLifecycleStatus::Cancelled ||
         status == OrderLifecycleStatus::Rejected;
}

[[nodiscard]] bool has_valid_top(const L2Book& book) {
  return book.best_bid() > 0.0 && book.best_ask() > 0.0 &&
         book.best_bid() <= book.best_ask();
}

[[nodiscard]] double visible_ask_queue_ahead(const L2Book& book,
                                             double sell_limit) {
  double queue = 0.0;
  for (const auto& level : book.asks) {
    if (level.price <= 0.0 || level.size <= 0.0)
      continue;
    if (level.price <= sell_limit + 1e-9)
      queue += level.size;
  }

  if (queue <= 0.0 && book.asks[0].size > 0.0) {
    queue = book.asks[0].size;
  }
  return std::max(queue, 1.0);
}

[[nodiscard]] double sell_directional_mu(const L2Book& book) {
  double bid_volume = 0.0;
  double ask_volume = 0.0;
  for (const auto& level : book.bids) {
    bid_volume += std::max(level.size, 0.0);
  }
  for (const auto& level : book.asks) {
    ask_volume += std::max(level.size, 0.0);
  }

  const double book_imbalance = imbalance(bid_volume, ask_volume);
  const double mid = 0.5 * (book.best_bid() + book.best_ask());
  const double mp = microprice(book.best_bid(), book.best_ask(),
                               book.bids[0].size, book.asks[0].size);
  const double micro_edge = (mid > 0.0) ? ((mp - mid) / mid) : 0.0;
  return std::clamp(book_imbalance + 100.0 * micro_edge, -1.0, 1.0);
}

}  // namespace

LiveExecutionEngine::LiveExecutionEngine(LiveTradingConfig cfg,
                                         std::unique_ptr<IBroker> broker)
    : cfg_(std::move(cfg)),
      broker_(std::move(broker)),
      ranking(cfg_.app.top_k, "shadow_results.csv") {}

bool LiveExecutionEngine::start() {
  hl::set_component_state(hl::ComponentId::Engine,
                          hl::ComponentState::Starting);
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
  refresh_order_state();
  ranking.step(t);

  if (!cfg_.app.order_enabled) {
    if ((t % 100) == 0) {
      hl::heartbeat(hl::ComponentId::Engine);
    }
    return;
  }

  if (!sync_next_order_id_from_broker()) {
    if ((t % 100) == 0) {
      hl::heartbeat(hl::ComponentId::Engine);
    }
    return;
  }

  refresh_order_state();
  route_exit_orders();

  for (const auto& s : ranking.portfolio.items) {
    if (!s.active)
      continue;
    if (has_open_exposure(s.symbol))
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
    entry_orders_[req.id] = EntryOrderState{s.symbol, req.qty, req.limit};
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

bool LiveExecutionEngine::has_open_exposure(const std::string& symbol) const {
  if (open_positions_.find(symbol) != open_positions_.end()) {
    return true;
  }
  for (const auto& item : entry_orders_) {
    if (item.second.symbol == symbol) {
      return true;
    }
  }
  return false;
}

bool LiveExecutionEngine::sync_next_order_id_from_broker() {
  auto* ibkr = dynamic_cast<IBKRClient*>(broker_.get());
  if (ibkr == nullptr) {
    return true;
  }

  const int next_valid_id = ibkr->next_valid_order_id();
  if (next_valid_id <= 0) {
    return false;
  }
  next_order_id_ = std::max(next_order_id_, next_valid_id);
  return true;
}

int LiveExecutionEngine::portfolio_index_for_symbol(
    const std::string& symbol) const {
  for (std::size_t i = 0; i < ranking.portfolio.items.size(); ++i) {
    if (ranking.portfolio.items[i].symbol == symbol) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

double LiveExecutionEngine::allocated_daily_cost_per_share() const {
  const double expected_shares = std::max(cfg_.app.expected_daily_shares, 1.0);
  const double daily_energy_cost = std::max(cfg_.app.daily_energy_kwh, 0.0) *
                                   std::max(cfg_.app.energy_cost_per_kwh, 0.0);
  const double daily_fixed_cost =
      daily_energy_cost + std::max(cfg_.app.daily_inflation_cost, 0.0);
  return daily_fixed_cost / expected_shares;
}

double LiveExecutionEngine::estimate_round_trip_cost_per_share(
    double qty, double entry_price, double sell_price_estimate) const {
  if (qty <= 0.0)
    return 0.0;

  InstitutionalTransactionCostModel costs(cfg_.app.commission_per_share,
                                          cfg_.app.half_spread_cost,
                                          cfg_.app.impact_coefficient);
  const double daily_volume = std::max(cfg_.app.assumed_daily_volume, qty);
  const double buy_cost =
      costs.estimateCost(0.0, qty, entry_price, daily_volume);
  const double sell_cost =
      costs.estimateCost(qty, 0.0, sell_price_estimate, daily_volume);
  return (buy_cost + sell_cost) / qty + allocated_daily_cost_per_share();
}

void LiveExecutionEngine::refresh_order_state() {
  auto* ibkr = dynamic_cast<IBKRClient*>(broker_.get());
  if (ibkr == nullptr)
    return;

  for (auto it = entry_orders_.begin(); it != entry_orders_.end();) {
    const auto* state = ibkr->lifecycle().get(it->first);
    if (state == nullptr) {
      ++it;
      continue;
    }

    if (state->status == OrderLifecycleStatus::Filled) {
      const double filled_qty =
          (state->filled_qty > 0.0) ? state->filled_qty : it->second.qty;
      const double entry_price = (state->avg_fill_price > 0.0)
                                     ? state->avg_fill_price
                                     : it->second.limit;
      auto& position = open_positions_[it->second.symbol];
      const double current_qty = position.qty;
      const double next_qty = current_qty + filled_qty;
      const double weighted_entry =
          (current_qty * position.entry_price + filled_qty * entry_price) /
          std::max(next_qty, 1e-9);
      position.symbol = it->second.symbol;
      position.qty = next_qty;
      position.entry_price = weighted_entry;
      position.entry_ack_latency_ms =
          std::max(ibkr->ack_latency_ms(it->first), 1.0);
      it = entry_orders_.erase(it);
      continue;
    }

    if (is_terminal_order_status(state->status)) {
      it = entry_orders_.erase(it);
      continue;
    }

    ++it;
  }

  for (auto it = exit_order_symbols_.begin();
       it != exit_order_symbols_.end();) {
    const auto* state = ibkr->lifecycle().get(it->first);
    if (state == nullptr) {
      ++it;
      continue;
    }

    auto pos = open_positions_.find(it->second);
    if (state->status == OrderLifecycleStatus::Filled) {
      if (pos != open_positions_.end()) {
        open_positions_.erase(pos);
      }
      it = exit_order_symbols_.erase(it);
      continue;
    }

    if (state->status == OrderLifecycleStatus::Cancelled ||
        state->status == OrderLifecycleStatus::Rejected) {
      if (pos != open_positions_.end()) {
        pos->second.sell_order_id = 0;
      }
      it = exit_order_symbols_.erase(it);
      continue;
    }

    ++it;
  }
}

void LiveExecutionEngine::route_exit_orders() {
  auto* ibkr = dynamic_cast<IBKRClient*>(broker_.get());
  if (ibkr == nullptr)
    return;

  for (auto& item : open_positions_) {
    auto& position = item.second;
    if (position.sell_order_id != 0)
      continue;

    const int idx = portfolio_index_for_symbol(position.symbol);
    if (idx < 0)
      continue;

    const auto book = ibkr->snapshot_book(idx + 1);
    if (!has_valid_top(book))
      continue;

    const auto& stock = ranking.portfolio.items[static_cast<std::size_t>(idx)];
    const double profit_pct = std::max(cfg_.app.target_profit_pct, 0.0);
    const double gross_target = position.entry_price * (1.0 + profit_pct);
    const double cost_per_share = estimate_round_trip_cost_per_share(
        position.qty, position.entry_price, gross_target);
    const double sell_limit = gross_target + cost_per_share;
    const double mid = 0.5 * (book.best_bid() + book.best_ask());
    const double queue_ahead = visible_ask_queue_ahead(book, sell_limit);
    const double latency_ms = std::max(
        {1.0, position.entry_ack_latency_ms, stock.latency.mean_latency()});
    const double net_reward = position.entry_price * profit_pct;
    const double loss = cost_per_share;
    const double sell_score =
        compute_execution_score(mid, sell_limit, sell_directional_mu(book),
                                std::max(stock.hawkes.lambda, 1e-9),
                                queue_ahead, latency_ms, net_reward, loss);

    position.sell_limit = sell_limit;
    position.sell_score = sell_score;
    if (sell_score < cfg_.app.min_sell_execution_score)
      continue;

    OrderRequest req;
    req.id = next_order_id_++;
    req.symbol = position.symbol;
    req.is_buy = false;
    req.qty = position.qty;
    req.limit = sell_limit;
    if (req.qty <= 0.0 || req.limit <= 0.0)
      continue;

    broker_->place_limit_order(req);
    position.sell_order_id = req.id;
    exit_order_symbols_[req.id] = position.symbol;
  }
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
