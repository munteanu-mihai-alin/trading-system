#include "engine/LiveExecutionEngine.hpp"
#include "broker/IBKRClient.hpp"
#include "execution/InstitutionalTransactionCostModel.h"
#include "execution/score.hpp"
#include "log/logging_state.hpp"
#include "models/l2_book.hpp"
#include "models/micro.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <ios>
#include <utility>

namespace hft {

namespace hl = hft::log;

namespace {

constexpr int kDepthTickerIdOffset = 100000;

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

// Wall-clock ns since Unix epoch, for the ts_ns column in every CSV row
// and for the session_start / session_end markers. system_clock is the
// right pick here: we want comparable timestamps across restarts and
// for "events on day X" queries; steady_clock isn't wall-clock.
[[nodiscard]] std::int64_t wall_ns_now() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

[[nodiscard]] std::string wall_iso_now() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto t = system_clock::to_time_t(now);
  std::tm gmt{};
#if defined(_WIN32)
  gmtime_s(&gmt, &t);
#else
  gmtime_r(&t, &gmt);
#endif
  const auto ms =
      duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
  char buf[40];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &gmt);
  char out[64];
  std::snprintf(out, sizeof(out), "%s.%03lldZ", buf,
                static_cast<long long>(ms));
  return out;
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
      ranking(cfg_.app.top_k, "shadow_results.csv", cfg_.app.shadow_enabled,
              cfg_.app.synthetic_fill_model, cfg_.app.entry_limit_mode) {
  // All four log schemas include a leading ts_ns column (wall-clock
  // ns) so events are queryable by day across restarts. open_log_ also
  // writes a session_start comment line so a second restart appended
  // to the same file shows the boundary.
  decision_log_ = open_log_(
      cfg_.app.decision_log_path,
      "ts_ns,step,decision_id,rank,symbol,score,score_tilt,hawkes_buy,"
      "hawkes_sell,hit_count,ou_mu,ou_initialized,mid,best_limit,active,"
      "chosen,gate\n",
      "decision");
  step_trace_log_ = open_log_(
      cfg_.app.step_trace_log_path,
      "ts_ns,step,decision_id,rank,symbol,score,score_tilt,hawkes_buy,"
      "hawkes_sell,hit_count,ou_mu,ou_initialized,mid,best_limit,active,"
      "chosen,gate\n",
      "step_trace");
  order_log_ =
      open_log_(cfg_.app.order_log_path,
                "ts_ns,step,order_id,symbol,side,qty,limit,event,filled_qty,"
                "remaining_qty,avg_fill_price\n",
                "order");
  l2_trace_log_ =
      open_log_(cfg_.app.l2_trace_log_path,
                "ts_ns,step,symbol,best_bid,bid_size,best_ask,ask_size,"
                "microprice,bid_vol_top10,ask_vol_top10,sell_limit,"
                "sell_score\n",
                "l2_trace");
}

std::unique_ptr<std::ofstream> LiveExecutionEngine::open_log_(
    const std::string& path, const char* header, const char* log_kind) {
  if (path.empty())
    return nullptr;
  const bool append = cfg_.app.log_append_mode;
  const bool exists = std::filesystem::exists(path);
  const auto sz = exists ? std::filesystem::file_size(path) : 0;
  const auto mode = std::ios::out | (append ? std::ios::app : std::ios::trunc);
  auto out = std::make_unique<std::ofstream>(path, mode);
  if (!out->is_open()) {
    return nullptr;
  }
  // Header only when the file is brand new (truncate, or append-but-empty).
  if (!append || sz == 0) {
    *out << header;
  }
  // Session boundary marker so multiple sessions in one file (append
  // mode) are visible. Pandas / CSV parsers can ignore '#' lines with
  // comment='#'.
  *out << "# session_start ts=" << wall_iso_now() << " ts_ns=" << wall_ns_now()
       << " kind=" << log_kind << " mode=" << cfg_.mode_name()
       << " label=" << (cfg_.app.run_label.empty() ? "-" : cfg_.app.run_label)
       << " universe_size=" << cfg_.app.universe_size << "\n";
  out->flush();
  return out;
}

void LiveExecutionEngine::close_logs_with_session_end_() {
  auto write_end = [&](std::ofstream* out, const char* kind) {
    if (out == nullptr || !out->is_open())
      return;
    *out << "# session_end ts=" << wall_iso_now() << " ts_ns=" << wall_ns_now()
         << " kind=" << kind << " orders_placed=" << orders_placed_
         << " open_positions=" << open_positions_.size() << "\n";
    out->flush();
  };
  write_end(decision_log_.get(), "decision");
  write_end(step_trace_log_.get(), "step_trace");
  write_end(order_log_.get(), "order");
  write_end(l2_trace_log_.get(), "l2_trace");
}

void LiveExecutionEngine::write_ranking_snapshot_to(
    std::ofstream& out, int decision_id, int t,
    const std::string& chosen_symbol, const std::string& gate) {
  const auto ts = wall_ns_now();
  for (std::size_t i = 0; i < ranking.portfolio.items.size(); ++i) {
    const auto& s = ranking.portfolio.items[i];
    const bool is_chosen =
        !chosen_symbol.empty() && (s.symbol == chosen_symbol);
    out << ts << ',' << t << ',' << decision_id << ',' << i << ',' << s.symbol
        << ',' << s.score << ',' << s.score_tilt << ',' << s.hawkes.lambda
        << ',' << s.hawkes_sell.lambda << ',' << s.hit_count << ',' << s.ou.mu
        << ',' << (s.ou_initialized ? 1 : 0) << ',' << s.mid << ','
        << s.best_limit << ',' << (s.active ? 1 : 0) << ','
        << (is_chosen ? 1 : 0) << ',' << (is_chosen ? "" : gate) << '\n';
  }
}

void LiveExecutionEngine::emit_decision_snapshot(
    int t, const std::string& chosen_symbol, const std::string& gate) {
  if (!decision_log_ || !decision_log_->is_open())
    return;
  write_ranking_snapshot_to(*decision_log_, next_decision_id_++, t,
                            chosen_symbol, gate);
  decision_log_->flush();
}

void LiveExecutionEngine::emit_order_event(
    int order_id, const std::string& symbol, const std::string& side,
    double qty, double limit, const std::string& event, double filled_qty,
    double remaining_qty, double avg_fill_price) {
  if (!order_log_ || !order_log_->is_open())
    return;
  *order_log_ << wall_ns_now() << ',' << current_step_t_ << ',' << order_id
              << ',' << symbol << ',' << side << ',' << qty << ',' << limit
              << ',' << event << ',' << filled_qty << ',' << remaining_qty
              << ',' << avg_fill_price << '\n';
  order_log_->flush();
}

void LiveExecutionEngine::emit_l2_trace(const std::string& symbol,
                                        const L2Book& book, double sell_limit,
                                        double sell_score) {
  if (!l2_trace_log_ || !l2_trace_log_->is_open())
    return;
  const double best_bid = book.best_bid();
  const double best_ask = book.best_ask();
  const double bid_sz0 = book.bids[0].size;
  const double ask_sz0 = book.asks[0].size;
  double microprice_value = 0.0;
  if (bid_sz0 > 0.0 && ask_sz0 > 0.0 && best_bid > 0.0 && best_ask > 0.0) {
    microprice_value =
        (ask_sz0 * best_bid + bid_sz0 * best_ask) / (bid_sz0 + ask_sz0);
  }
  double bid_vol = 0.0;
  double ask_vol = 0.0;
  for (const auto& level : book.bids)
    bid_vol += std::max(level.size, 0.0);
  for (const auto& level : book.asks)
    ask_vol += std::max(level.size, 0.0);
  *l2_trace_log_ << wall_ns_now() << ',' << current_step_t_ << ',' << symbol
                 << ',' << best_bid << ',' << bid_sz0 << ',' << best_ask << ','
                 << ask_sz0 << ',' << microprice_value << ',' << bid_vol << ','
                 << ask_vol << ',' << sell_limit << ',' << sell_score << '\n';
  l2_trace_log_->flush();
}

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
  // Mark every open log with a session_end row so subsequent reads can
  // see exactly when this session finished. In append mode the next
  // session will write its own session_start; the gap between the two
  // timestamps is the system's downtime.
  close_logs_with_session_end_();
}

void LiveExecutionEngine::initialize_universe(int n_stocks) {
  hl::set_component_state(hl::ComponentId::Universe,
                          hl::ComponentState::Starting);
  ranking.initialize(n_stocks);
  hl::set_component_state(hl::ComponentId::Universe, hl::ComponentState::Ready);
}

void LiveExecutionEngine::step(int t) {
  current_step_t_ = t;
  broker_->on_step(t);
  reconcile_broker_state();
  refresh_order_state();
  ranking.step(t);

  // Per-step ranking snapshot to the step-trace file (separate from the
  // buy-event decisions file). No chosen symbol, no gate - this is the
  // full ranking at end-of-step for time-series analysis.
  if (step_trace_log_ && step_trace_log_->is_open()) {
    write_ranking_snapshot_to(*step_trace_log_, next_step_trace_id_++, t,
                              /*chosen=*/"", /*gate=*/"");
    step_trace_log_->flush();
  }

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

  // Compute per-symbol target notional once per step (equal vs
  // score_weighted vs rank_weighted, etc.). Picked up below.
  const auto per_symbol_notional = compute_per_symbol_notional();

  for (const auto& s : ranking.portfolio.items) {
    if (!s.active)
      continue;
    if (!can_open_new_exposure())
      break;
    if (has_open_exposure(s.symbol))
      continue;
    if (!can_route_order(s))
      continue;
    // Mean-reversion entry gate: only buy when mid is at-or-below the
    // trailing OU mean by at least ou_buy_threshold_pct. Disabled when
    // both ou_halflife_seconds <= 0 and ou_window_size == 0, or when the
    // OU state has not yet been primed by a real mid observation.
    const bool ou_gate_active =
        (cfg_.app.ou_halflife_seconds > 0.0 || cfg_.app.ou_window_size > 0);
    if (ou_gate_active && s.ou_initialized) {
      const double mu_cap = s.ou.mu * (1.0 + cfg_.app.ou_buy_threshold_pct);
      if (s.mid > mu_cap)
        continue;
    }

    OrderRequest req;
    req.id = next_order_id_++;
    req.symbol = s.symbol;
    req.is_buy = true;
    req.limit = s.best_limit;
    if (req.limit <= 0.0) {
      --next_order_id_;
      continue;
    }
    double target_notional = cfg_.app.trade_notional;
    const auto pit = per_symbol_notional.find(s.symbol);
    if (pit != per_symbol_notional.end()) {
      target_notional = pit->second;
    }
    req.qty = size_entry_qty(req.limit, target_notional);
    if (req.qty <= 0.0) {
      // Either target_notional < price (e.g. 500 budget on a 700 stock) or
      // legacy order_qty knobs zeroed it out. Skip without burning the id.
      --next_order_id_;
      continue;
    }
    const double order_notional = req.qty * req.limit;
    if (cfg_.app.max_notional_per_order > 0.0 &&
        order_notional > cfg_.app.max_notional_per_order) {
      --next_order_id_;
      continue;
    }
    // Account-level budget gate: pending + open + this order must not exceed
    // account_budget. When account_budget <= 0 the gate is disabled.
    if (cfg_.app.account_budget > 0.0 &&
        committed_notional() + order_notional > cfg_.app.account_budget) {
      --next_order_id_;
      break;  // budget exhausted - no point checking the rest this step
    }
    emit_decision_snapshot(t, s.symbol, /*gate=*/"");
    broker_->place_limit_order(req);
    emit_order_event(req.id, req.symbol, "buy", req.qty, req.limit, "placed",
                     /*filled_qty=*/0.0, /*remaining_qty=*/req.qty,
                     /*avg_fill_price=*/0.0);
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

int LiveExecutionEngine::open_exposure_symbol_count() const {
  std::unordered_set<std::string> symbols;
  for (const auto& item : open_positions_) {
    symbols.insert(item.first);
  }
  for (const auto& item : entry_orders_) {
    symbols.insert(item.second.symbol);
  }
  return static_cast<int>(symbols.size());
}

bool LiveExecutionEngine::can_open_new_exposure() const {
  if (cfg_.app.max_open_symbols <= 0) {
    return true;
  }
  return open_exposure_symbol_count() < cfg_.app.max_open_symbols;
}

double LiveExecutionEngine::committed_notional() const {
  double total = 0.0;
  for (const auto& item : entry_orders_) {
    total += item.second.qty * item.second.limit;
  }
  for (const auto& item : open_positions_) {
    total += item.second.qty * item.second.entry_price;
  }
  return total;
}

double LiveExecutionEngine::size_entry_qty(double limit_price,
                                           double target_notional) const {
  if (limit_price <= 0.0)
    return 0.0;
  // Notional-driven sizing wins when configured (default 500 per trade,
  // or a per-symbol share when position_sizing_rule != "equal").
  if (target_notional > 0.0) {
    const double raw = target_notional / limit_price;
    double qty = std::floor(raw);
    if (cfg_.app.max_order_qty > 0.0) {
      qty = std::min(qty, std::floor(cfg_.app.max_order_qty));
    }
    return qty;
  }
  // Legacy fixed-share path: order_qty floored by max_order_qty.
  double qty = cfg_.app.order_qty;
  if (cfg_.app.max_order_qty > 0.0) {
    qty = std::min(qty, cfg_.app.max_order_qty);
  }
  return qty;
}

std::unordered_map<std::string, double>
LiveExecutionEngine::compute_per_symbol_notional() const {
  std::unordered_map<std::string, double> out;
  const auto& items = ranking.portfolio.items;
  if (cfg_.app.position_sizing_rule == "score_weighted" &&
      cfg_.app.account_budget > 0.0) {
    double total_score = 0.0;
    for (const auto& s : items) {
      if (!s.active)
        continue;
      total_score += std::max(s.score, 0.0);
    }
    if (total_score > 0.0) {
      for (const auto& s : items) {
        if (!s.active)
          continue;
        const double weight = std::max(s.score, 0.0) / total_score;
        out[s.symbol] = cfg_.app.account_budget * weight;
      }
      return out;
    }
    // No positive score among active items: fall through to equal sizing.
  }
  for (const auto& s : items) {
    if (!s.active)
      continue;
    out[s.symbol] = cfg_.app.trade_notional;
  }
  return out;
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

void LiveExecutionEngine::ensure_depth_subscription(const std::string& symbol,
                                                    int ticker_id) {
  if (depth_subscribed_symbols_.find(symbol) != depth_subscribed_symbols_.end())
    return;
  broker_->subscribe_market_depth(MarketDepthRequest{ticker_id, symbol, 5});
  depth_subscribed_symbols_.insert(symbol);
}

void LiveExecutionEngine::refresh_order_state() {
  const auto* lifecycle = broker_->order_lifecycle();
  if (lifecycle == nullptr)
    return;

  for (auto it = entry_orders_.begin(); it != entry_orders_.end();) {
    const auto* state = lifecycle->get(it->first);
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
      emit_order_event(it->first, it->second.symbol, "buy", it->second.qty,
                       it->second.limit, "filled", filled_qty, 0.0,
                       entry_price);
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
          std::max(broker_->ack_latency_ms(it->first), 1.0);
      it = entry_orders_.erase(it);
      continue;
    }

    if (is_terminal_order_status(state->status)) {
      const char* event = state->status == OrderLifecycleStatus::Cancelled
                              ? "cancelled"
                              : "rejected";
      emit_order_event(it->first, it->second.symbol, "buy", it->second.qty,
                       it->second.limit, event, state->filled_qty,
                       state->remaining_qty, state->avg_fill_price);
      it = entry_orders_.erase(it);
      continue;
    }

    ++it;
  }

  for (auto it = exit_order_symbols_.begin();
       it != exit_order_symbols_.end();) {
    const auto* state = lifecycle->get(it->first);
    if (state == nullptr) {
      ++it;
      continue;
    }

    auto pos = open_positions_.find(it->second);
    // Resolve the sell-side qty/limit before we potentially erase the
    // position; needed so the order-log row has full context even on
    // Filled.
    const double sell_qty = (pos != open_positions_.end()) ? pos->second.qty
                            : (state->requested_qty > 0.0)
                                ? state->requested_qty
                                : 0.0;
    const double sell_limit_px =
        (pos != open_positions_.end()) ? pos->second.sell_limit : 0.0;
    if (state->status == OrderLifecycleStatus::Filled) {
      emit_order_event(it->first, it->second, "sell", sell_qty, sell_limit_px,
                       "filled",
                       state->filled_qty > 0.0 ? state->filled_qty : sell_qty,
                       0.0, state->avg_fill_price);
      if (pos != open_positions_.end()) {
        open_positions_.erase(pos);
      }
      it = exit_order_symbols_.erase(it);
      continue;
    }

    if (state->status == OrderLifecycleStatus::Cancelled ||
        state->status == OrderLifecycleStatus::Rejected) {
      const char* event = state->status == OrderLifecycleStatus::Cancelled
                              ? "cancelled"
                              : "rejected";
      emit_order_event(it->first, it->second, "sell", sell_qty, sell_limit_px,
                       event, state->filled_qty, state->remaining_qty,
                       state->avg_fill_price);
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
  const auto* lifecycle = broker_->order_lifecycle();
  if (lifecycle == nullptr)
    return;

  for (auto& item : open_positions_) {
    auto& position = item.second;
    if (position.sell_order_id != 0)
      continue;

    const int idx = portfolio_index_for_symbol(position.symbol);
    if (idx < 0)
      continue;

    const int depth_ticker_id = kDepthTickerIdOffset + idx + 1;
    ensure_depth_subscription(position.symbol, depth_ticker_id);
    const auto book = broker_->snapshot_book(depth_ticker_id);
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
    // Two-channel Hawkes: use the SELL-aggressor intensity for the
    // execution score on exits, falling back to the buy channel if the
    // sell channel hasn't seen any classified events yet (would still
    // be at the default mu=10 baseline).
    const double lambda_for_exit =
        std::max(stock.hawkes_sell.lambda, std::max(stock.hawkes.lambda, 1e-9));
    const double sell_score = compute_execution_score(
        mid, sell_limit, sell_directional_mu(book), lambda_for_exit,
        queue_ahead, latency_ms, net_reward, loss);

    position.sell_limit = sell_limit;
    position.sell_score = sell_score;
    // Per-step L2 trace for this held symbol (whether or not we end up
    // submitting the sell this step). Captures the microstructure state
    // the sell score was computed from.
    emit_l2_trace(position.symbol, book, sell_limit, sell_score);
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
    emit_order_event(req.id, req.symbol, "sell", req.qty, req.limit, "placed",
                     /*filled_qty=*/0.0, /*remaining_qty=*/req.qty,
                     /*avg_fill_price=*/0.0);
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
    broker_->subscribe_top_of_book(TopOfBookRequest{ticker, sym});
    if (cfg_.app.hawkes_use_real_trades) {
      broker_->subscribe_trades(TopOfBookRequest{ticker, sym});
    }
    ++ticker;
  }
  hl::set_component_state(hl::ComponentId::MarketData,
                          hl::ComponentState::Ready);
}

void LiveExecutionEngine::update_hit_count_tilt() {
  if (!cfg_.app.hit_count_enabled)
    return;
  if (cfg_.app.hit_count_horizon_seconds <= 0.0)
    return;
  const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
  const std::int64_t horizon_ns =
      static_cast<std::int64_t>(cfg_.app.hit_count_horizon_seconds * 1e9);
  const double baseline = std::max(cfg_.app.hit_count_baseline, 1.0);
  for (auto& s : ranking.portfolio.items) {
    if (s.mid <= 0.0)
      continue;
    if (s.window_open_ts_ns == 0) {
      // Bootstrap: open the first window at the current observation.
      s.window_open_mid = s.mid;
      s.window_open_ts_ns = now_ns;
      continue;
    }
    if (now_ns - s.window_open_ts_ns < horizon_ns)
      continue;
    if (s.window_open_mid > 0.0) {
      const double ret = (s.mid - s.window_open_mid) / s.window_open_mid;
      if (ret >= cfg_.app.hit_count_target_pct) {
        ++s.hit_count;
      }
    }
    // Always slide the window forward, regardless of hit/miss.
    s.window_open_mid = s.mid;
    s.window_open_ts_ns = now_ns;
    const double raw_tilt = static_cast<double>(s.hit_count) / baseline;
    s.score_tilt = std::clamp(raw_tilt, cfg_.app.hit_count_tilt_min,
                              cfg_.app.hit_count_tilt_max);
  }
}

void LiveExecutionEngine::update_hawkes_from_trades() {
  if (!cfg_.app.hawkes_use_real_trades)
    return;
  for (std::size_t i = 0; i < ranking.portfolio.items.size(); ++i) {
    auto& s = ranking.portfolio.items[i];
    const int ticker_id = static_cast<int>(i + 1);
    const auto trades = broker_->drain_trades(ticker_id);
    if (trades.empty())
      continue;
    // Lee-Ready aggressor classification against the most-recent top-of-book
    // snapshot. price >= ask -> buy-aggressor (lifts s.hawkes), price <= bid
    // -> sell-aggressor (lifts s.hawkes_sell). Mid-of-spread trades have no
    // reliable classification at this seam (no prior-trade tick rule yet),
    // so they update neither channel - safer than guessing.
    const auto top = broker_->snapshot_top_of_book(ticker_id);
    const double bid = top.bid_price;
    const double ask = top.ask_price;
    const bool top_valid = bid > 0.0 && ask > 0.0 && bid <= ask;
    for (const auto& t : trades) {
      double dt_s = 0.001;
      if (s.last_trade_ts_ns > 0 && t.exch_ts_ns > s.last_trade_ts_ns) {
        dt_s = static_cast<double>(t.exch_ts_ns - s.last_trade_ts_ns) * 1e-9;
      }
      if (top_valid) {
        if (t.price >= ask) {
          s.hawkes.update(dt_s, /*event=*/1);
        } else if (t.price <= bid) {
          s.hawkes_sell.update(dt_s, /*event=*/1);
        } else {
          // Mid-of-spread trade; classification ambiguous. Update neither
          // channel but still advance last_trade_ts_ns below so dt for the
          // next event is sensible.
        }
      } else {
        // No usable quote context (e.g. early ticks before top-of-book
        // arrived). Fall back to single-channel behaviour on the buy side
        // so we don't lose every event in the bootstrap window.
        s.hawkes.update(dt_s, /*event=*/1);
      }
      s.last_trade_ts_ns = t.exch_ts_ns;
    }
  }
}

void LiveExecutionEngine::reconcile_broker_state() {
  update_hawkes_from_trades();
  update_hit_count_tilt();
  for (std::size_t i = 0; i < ranking.portfolio.items.size(); ++i) {
    auto& s = ranking.portfolio.items[i];
    const auto top = broker_->snapshot_top_of_book(static_cast<int>(i + 1));
    if (top.valid()) {
      s.mid = top.mid();
      // Plumb the raw L1 bid/ask through so RankingEngine can set
      // best_limit realistically (marketable at ask vs passive at mid)
      // under AppConfig::entry_limit_mode.
      s.bid_price = top.bid_price;
      s.ask_price = top.ask_price;
      if (top.bid_size > 0.0) {
        s.queue = top.bid_size;
      }
      // Hawkes mid-change proxy: when configured, fire a Hawkes event
      // each time the mid moves by at least the threshold (bps,
      // relative to the last firing). Provides a real-data-driven
      // intensity signal without paying Databento for the trades
      // schema. Off by default.
      if (cfg_.app.hawkes_mid_change_threshold_bps > 0.0 && s.mid > 0.0) {
        if (s.last_mid_for_hawkes <= 0.0) {
          s.last_mid_for_hawkes = s.mid;
        } else {
          const double change_bps = 10000.0 *
                                    std::abs(s.mid - s.last_mid_for_hawkes) /
                                    s.last_mid_for_hawkes;
          if (change_bps >= cfg_.app.hawkes_mid_change_threshold_bps) {
            // dt approximated as 1ms; mid updates in backtest are
            // step-driven and bursts of changes within a step still
            // get distinct events.
            s.hawkes.update(0.001, /*event=*/1);
            s.last_mid_for_hawkes = s.mid;
          }
        }
      }
      // EWMA toward the observed mid so ou.mu tracks the trailing mean.
      // Two parameterisations:
      //   1. ou_halflife_seconds > 0: dt-weighted EWMA in wall-clock time
      //      via alpha = 1 - exp(-dt_s / tau), tau = halflife / ln 2.
      //      Consistent across symbols of different update rates.
      //   2. ou_window_size > 0 (legacy): sample-count EWMA with
      //      alpha = 1 / window_size.
      // First observation bootstraps ou.mu to the current mid so the gate
      // is not pinned to the default 100.0 for non-$100 symbols.
      const bool ou_enabled =
          cfg_.app.ou_halflife_seconds > 0.0 || cfg_.app.ou_window_size > 0;
      if (ou_enabled && s.mid > 0.0) {
        const auto now_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
        if (!s.ou_initialized) {
          s.ou.mu = s.mid;
          s.ou.x = s.mid;
          s.ou_initialized = true;
          s.last_ou_update_ts_ns = now_ns;
        } else {
          double alpha = 0.0;
          if (cfg_.app.ou_halflife_seconds > 0.0) {
            const double dt_s =
                (s.last_ou_update_ts_ns > 0 && now_ns > s.last_ou_update_ts_ns)
                    ? static_cast<double>(now_ns - s.last_ou_update_ts_ns) *
                          1e-9
                    : 0.0;
            const double tau = cfg_.app.ou_halflife_seconds / std::log(2.0);
            alpha = 1.0 - std::exp(-dt_s / std::max(tau, 1e-9));
          } else {
            alpha = 1.0 / static_cast<double>(cfg_.app.ou_window_size);
          }
          s.ou.mu = (1.0 - alpha) * s.ou.mu + alpha * s.mid;
          update_ou(s.ou, s.mid);
          s.last_ou_update_ts_ns = now_ns;
        }
      }
    }
  }
  ranking.portfolio.rank();
}

}  // namespace hft
