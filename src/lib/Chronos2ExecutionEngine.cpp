#include "engine/Chronos2ExecutionEngine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

#include "broker/OrderLifecycle.hpp"

namespace hft {

namespace {

// Match the Databento downloader convention (see
// DatabentoBacktestBroker::l2_downloader_command): wrap paths for
// std::system with double-quotes so spaces are OK.
std::string quote(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\')
      out.push_back('\\');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

std::string today_yyyymmdd() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[16];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
  return std::string(buf);
}

double sanitize(double x) {
  if (!std::isfinite(x))
    return 0.0;
  return x;
}

}  // namespace

Chronos2ExecutionEngine::Chronos2ExecutionEngine(
    LiveTradingConfig cfg, std::unique_ptr<IBroker> broker)
    : cfg_(std::move(cfg)), broker_(std::move(broker)) {
  next_reinvest_threshold_ = cfg_.app.chronos2_reinvest_increment;
}

Chronos2ExecutionEngine::~Chronos2ExecutionEngine() {
  if (started_)
    stop();
}

bool Chronos2ExecutionEngine::start() {
  if (!broker_->connect(cfg_.app.host, cfg_.app.paper_port,
                        cfg_.app.client_id)) {
    std::cerr << "[chronos2] broker connect failed" << std::endl;
    return false;
  }
  broker_->start_event_loop();
  started_ = true;
  return true;
}

void Chronos2ExecutionEngine::stop() {
  if (!started_)
    return;
  broker_->stop_event_loop();
  broker_->disconnect();
  started_ = false;
}

void Chronos2ExecutionEngine::initialize_universe(
    const std::vector<std::pair<std::string, std::string>>& list,
    int n_stocks) {
  portfolio_.items.clear();
  const int count = std::min<int>(n_stocks, static_cast<int>(list.size()));
  for (int i = 0; i < count; ++i) {
    Stock s;
    s.symbol = list[i].first;
    s.company = list[i].second;
    s.mid = 0.0;
    portfolio_.items.push_back(s);
  }
}

void Chronos2ExecutionEngine::subscribe_live_books() {
  for (std::size_t i = 0; i < portfolio_.items.size(); ++i) {
    TopOfBookRequest req;
    req.ticker_id = static_cast<int>(i) + 1;
    req.symbol = portfolio_.items[i].symbol;
    broker_->subscribe_top_of_book(req);
  }
}

// ---- Main loop ----

void Chronos2ExecutionEngine::step(int t) {
  broker_->on_step(t);
  reconcile_broker_state();
  refresh_order_state();

  if (!cfg_.app.order_enabled)
    return;

  update_daily_close_history();
  maybe_load_chronos_predictions();
  compute_composite_scores();
  portfolio_.rank();

  route_exit_orders();
  route_entries();
}

// ---- Broker sync ----

void Chronos2ExecutionEngine::reconcile_broker_state() {
  // Pull top-of-book snapshots and update per-symbol mid.
  for (std::size_t i = 0; i < portfolio_.items.size(); ++i) {
    const int ticker_id = static_cast<int>(i) + 1;
    const auto top = broker_->snapshot_top_of_book(ticker_id);
    if (!top.valid())
      continue;
    auto& s = portfolio_.items[i];
    s.bid_price = top.bid_price;
    s.ask_price = top.ask_price;
    if (s.bid_price > 0.0 && s.ask_price > 0.0) {
      s.mid = 0.5 * (s.bid_price + s.ask_price);
    }
  }
  // Trade events aren't consumed by this strategy -- we don't run
  // Hawkes here. drain_trades() is per-ticker (see IBroker), and this
  // engine doesn't score off individual trades. Skipping the drain
  // means the broker's per-ticker trade queue keeps whatever it kept;
  // for the backtest brokers this is benign (queues are bounded), for
  // live brokers add a per-symbol drain loop if backpressure ever
  // becomes an issue.
}

void Chronos2ExecutionEngine::refresh_order_state() {
  const auto* lifecycle = broker_->order_lifecycle();
  if (!lifecycle)
    return;

  // Buy fills: scan pending entry orders.
  for (auto it = entry_orders_.begin(); it != entry_orders_.end();) {
    const auto* state = lifecycle->get(it->first);
    if (!state) {
      ++it;
      continue;
    }
    if (state->status == OrderLifecycleStatus::Filled) {
      handle_buy_fill(it->first, state->avg_fill_price, state->filled_qty);
      it = entry_orders_.erase(it);
    } else if (state->status == OrderLifecycleStatus::Cancelled ||
               state->status == OrderLifecycleStatus::Rejected) {
      it = entry_orders_.erase(it);
    } else {
      ++it;
    }
  }

  // Sell fills: scan open positions with a pending exit.
  for (auto it = exit_order_symbols_.begin();
       it != exit_order_symbols_.end();) {
    const auto* state = lifecycle->get(it->first);
    if (!state) {
      ++it;
      continue;
    }
    if (state->status == OrderLifecycleStatus::Filled) {
      handle_sell_fill(it->first, state->avg_fill_price, state->filled_qty);
      it = exit_order_symbols_.erase(it);
    } else if (state->status == OrderLifecycleStatus::Cancelled ||
               state->status == OrderLifecycleStatus::Rejected) {
      auto pos = open_positions_.find(it->second);
      if (pos != open_positions_.end()) {
        pos->second.sell_order_id = 0;
        pos->second.sell_limit = 0.0;
      }
      it = exit_order_symbols_.erase(it);
    } else {
      ++it;
    }
  }
}

void Chronos2ExecutionEngine::handle_buy_fill(int order_id, double fill_price,
                                              double filled_qty) {
  auto oit = entry_orders_.find(order_id);
  if (oit == entry_orders_.end())
    return;
  const std::string symbol = oit->second.symbol;

  OpenPositionState pos;
  pos.symbol = symbol;
  pos.qty = filled_qty;
  pos.entry_price = fill_price > 0.0 ? fill_price : oit->second.limit;

  // Capture the Chronos-2 predicted price AT FILL so a later daily
  // forecast that lowers its guess doesn't shrink our exit target.
  const int idx = portfolio_index_for_symbol(symbol);
  if (idx >= 0) {
    pos.predicted_price_at_fill = portfolio_.items[idx].predicted_price;
  }
  open_positions_[symbol] = pos;
}

void Chronos2ExecutionEngine::handle_sell_fill(int order_id, double fill_price,
                                               double filled_qty) {
  (void)order_id;
  auto sit = exit_order_symbols_.find(order_id);
  if (sit == exit_order_symbols_.end())
    return;
  const std::string symbol = sit->second;

  auto pit = open_positions_.find(symbol);
  if (pit != open_positions_.end()) {
    const double entry = pit->second.entry_price;
    const double px = fill_price > 0.0 ? fill_price : pit->second.sell_limit;
    realized_pnl_ += (px - entry) * filled_qty;
    open_positions_.erase(pit);
  }

  // Reinvest schedule: cross each threshold of realized profit ->
  // bump bonus_budget_ by one increment.
  const double inc = cfg_.app.chronos2_reinvest_increment;
  while (inc > 0.0 && realized_pnl_ >= next_reinvest_threshold_) {
    bonus_budget_ += inc;
    next_reinvest_threshold_ += inc;
  }
}

// ---- Chronos-2 daily forecast bridge ----

void Chronos2ExecutionEngine::update_daily_close_history() {
  // Cheap: once per trading day, append current mid to each symbol's
  // rolling window. We snapshot at every step; the write-through only
  // happens when the day rolls in maybe_load_chronos_predictions.
  // Use "date" key change; if day is same, still no-op.
  static thread_local std::string last_day;
  const std::string today = today_yyyymmdd();
  if (today == last_day)
    return;
  last_day = today;

  const int cap = std::max(1, cfg_.app.chronos2_context_len * 2);
  for (const auto& s : portfolio_.items) {
    if (s.mid <= 0.0)
      continue;
    auto& hist = daily_closes_[s.symbol];
    hist.push_back(s.mid);
    if (static_cast<int>(hist.size()) > cap) {
      hist.erase(hist.begin(), hist.end() - cap);
    }
  }
}

void Chronos2ExecutionEngine::maybe_load_chronos_predictions() {
  const std::string today = today_yyyymmdd();
  if (today == last_load_yyyymmdd_)
    return;

  // Skip until we have at least CHRONOS_CONTEXT_LEN closes for something.
  const int need = cfg_.app.chronos2_context_len;
  bool any_ready = false;
  for (const auto& kv : daily_closes_) {
    if (static_cast<int>(kv.second.size()) >= need) {
      any_ready = true;
      break;
    }
  }
  if (!any_ready)
    return;

  std::filesystem::create_directories(cfg_.app.chronos2_daily_closes_dir);
  std::filesystem::create_directories(cfg_.app.chronos2_predictions_dir);
  const std::string in_csv =
      cfg_.app.chronos2_daily_closes_dir + "/history_" + today + ".csv";
  const std::string out_csv =
      cfg_.app.chronos2_predictions_dir + "/predictions_" + today + ".csv";

  write_daily_closes_csv(in_csv);
  const int rc = spawn_chronos_forecast(in_csv, out_csv);
  if (rc != 0) {
    std::cerr << "[chronos2] forecast subprocess rc=" << rc
              << " (predictions stale)" << std::endl;
    return;  // keep previous predictions; try again tomorrow
  }
  read_predictions_csv(out_csv);
  last_load_yyyymmdd_ = today;
}

void Chronos2ExecutionEngine::write_daily_closes_csv(
    const std::string& out_path) const {
  std::ofstream f(out_path);
  if (!f)
    return;
  f << "symbol,date,close\n";
  // Reconstruct a fake date per row (relative index) -- Python side
  // sorts by (symbol, date) but doesn't otherwise use the value, so
  // a monotonic string is enough.
  for (const auto& kv : daily_closes_) {
    const auto& sym = kv.first;
    const auto& closes = kv.second;
    for (std::size_t i = 0; i < closes.size(); ++i) {
      char idxbuf[32];
      std::snprintf(idxbuf, sizeof(idxbuf), "%010zu", i);
      f << sym << ',' << idxbuf << ',' << closes[i] << '\n';
    }
  }
}

int Chronos2ExecutionEngine::spawn_chronos_forecast(
    const std::string& in_csv, const std::string& out_csv) const {
  std::ostringstream cmd;
  cmd << quote(cfg_.app.chronos2_python) << ' '
      << quote(cfg_.app.chronos2_forecast_script) << " --history-csv "
      << quote(in_csv) << " --output " << quote(out_csv) << " --model "
      << quote(cfg_.app.chronos2_model) << " --context-len "
      << cfg_.app.chronos2_context_len << " --prediction-len "
      << cfg_.app.chronos2_prediction_len << " --vol-lookback "
      << cfg_.app.chronos2_vol_lookback_days << " --momentum-lookback "
      << cfg_.app.chronos2_momentum_lookback_days;
  return std::system(cmd.str().c_str());
}

void Chronos2ExecutionEngine::read_predictions_csv(const std::string& path) {
  std::ifstream f(path);
  if (!f)
    return;

  std::string header;
  if (!std::getline(f, header))
    return;
  // Expected header: symbol,predicted_price,predicted_q25,realized_vol,momentum_5d

  std::string line;
  while (std::getline(f, line)) {
    if (line.empty())
      continue;
    std::stringstream ss(line);
    std::string sym, pp, pq, vol, mom;
    if (!std::getline(ss, sym, ','))
      continue;
    if (!std::getline(ss, pp, ','))
      continue;
    if (!std::getline(ss, pq, ','))
      continue;
    if (!std::getline(ss, vol, ','))
      continue;
    if (!std::getline(ss, mom, ','))
      continue;

    const int idx = portfolio_index_for_symbol(sym);
    if (idx < 0)
      continue;
    auto& s = portfolio_.items[idx];
    try {
      s.predicted_price = sanitize(std::stod(pp));
      s.predicted_q25 = sanitize(std::stod(pq));
      s.realized_vol = sanitize(std::stod(vol));
      s.momentum_5d = sanitize(std::stod(mom));
    } catch (const std::exception&) {
      s.predicted_price = 0.0;
      s.predicted_q25 = 0.0;
      s.realized_vol = 0.0;
      s.momentum_5d = 0.0;
    }
  }
}

// ---- Scoring + routing ----

void Chronos2ExecutionEngine::compute_composite_scores() {
  const double floor_v = std::max(1e-6, cfg_.app.chronos2_vol_floor);
  for (auto& s : portfolio_.items) {
    if (s.mid <= 0.0 || s.predicted_price <= 0.0) {
      s.score = -1e18;
      s.active = false;
      continue;
    }
    const double predicted_ret = (s.predicted_price - s.mid) / s.mid;
    s.score = predicted_ret / std::max(s.realized_vol, floor_v);
  }
}

void Chronos2ExecutionEngine::route_entries() {
  const int top_k = effective_top_k();
  const double bud = effective_budget();
  double committed = committed_notional();
  int considered = 0;

  for (const int idx : portfolio_.ranked_indices) {
    if (considered++ >= top_k)
      break;
    const auto& s = portfolio_.items[idx];
    if (open_positions_.count(s.symbol) > 0)
      continue;
    // Also skip if we already have a pending entry for this symbol.
    bool pending = false;
    for (const auto& kv : entry_orders_) {
      if (kv.second.symbol == s.symbol) {
        pending = true;
        break;
      }
    }
    if (pending)
      continue;
    if (s.mid <= 0.0 || s.predicted_price <= 0.0)
      continue;

    // ---- Chronos-2 entry filters (mirror Python) ----
    const double predicted_ret = (s.predicted_price - s.mid) / s.mid;
    const double q25_ret =
        s.predicted_q25 > 0.0 ? (s.predicted_q25 - s.mid) / s.mid : 0.0;
    if (predicted_ret < cfg_.app.target_profit_pct)
      continue;
    if (q25_ret <= 0.0)
      continue;
    if (s.realized_vol > cfg_.app.chronos2_max_annual_vol)
      continue;
    if (s.momentum_5d < 0.0)
      continue;

    // Budget gate.
    if (committed + cfg_.app.trade_notional > bud)
      break;

    // Size + place a marketable limit at the current ask.
    const double px = s.ask_price > 0.0 ? s.ask_price : s.mid;
    const int qty = static_cast<int>(
        std::floor(cfg_.app.trade_notional / std::max(1e-9, px)));
    if (qty <= 0)
      continue;

    OrderRequest req;
    req.id = next_order_id_++;
    req.symbol = s.symbol;
    req.is_buy = true;
    req.qty = static_cast<double>(qty);
    req.limit = px;
    broker_->place_limit_order(req);

    entry_orders_[req.id] = EntryOrderState{s.symbol, req.qty, req.limit};
    committed += qty * px;
  }
}

void Chronos2ExecutionEngine::route_exit_orders() {
  for (auto& kv : open_positions_) {
    auto& pos = kv.second;
    if (pos.qty <= 0.0)
      continue;
    if (pos.sell_order_id != 0)
      continue;  // sell already working

    const int idx = portfolio_index_for_symbol(pos.symbol);
    if (idx < 0)
      continue;
    const auto& s = portfolio_.items[idx];

    // Chronos-driven target with never-sell-at-loss fallback:
    //   target = max(predicted_price, entry * (1 + target_profit_pct))
    const double fallback =
        pos.entry_price * (1.0 + std::max(0.0, cfg_.app.target_profit_pct));
    double target = fallback;
    if (pos.predicted_price_at_fill > 0.0) {
      target = std::max(pos.predicted_price_at_fill, fallback);
    } else if (s.predicted_price > 0.0) {
      target = std::max(s.predicted_price, fallback);
    }

    OrderRequest req;
    req.id = next_order_id_++;
    req.symbol = pos.symbol;
    req.is_buy = false;
    req.qty = pos.qty;
    req.limit = target;
    broker_->place_limit_order(req);

    pos.sell_order_id = req.id;
    pos.sell_limit = target;
    exit_order_symbols_[req.id] = pos.symbol;
  }
}

// ---- Reinvest schedule + capacity ----

int Chronos2ExecutionEngine::effective_top_k() const {
  const double inc = cfg_.app.chronos2_reinvest_increment;
  const int base = cfg_.app.top_k > 0 ? cfg_.app.top_k : 3;
  if (inc <= 0.0)
    return base;
  const int gained = static_cast<int>(std::floor(bonus_budget_ / inc));
  return base + gained;
}

double Chronos2ExecutionEngine::effective_budget() const {
  return cfg_.app.account_budget + bonus_budget_;
}

double Chronos2ExecutionEngine::committed_notional() const {
  double total = 0.0;
  for (const auto& kv : open_positions_) {
    total += kv.second.qty * kv.second.entry_price;
  }
  for (const auto& kv : entry_orders_) {
    total += kv.second.qty * kv.second.limit;
  }
  return total;
}

bool Chronos2ExecutionEngine::has_free_slot() const {
  return committed_notional() + cfg_.app.trade_notional <= effective_budget();
}

// ---- Helpers ----

int Chronos2ExecutionEngine::portfolio_index_for_symbol(
    const std::string& symbol) const {
  for (std::size_t i = 0; i < portfolio_.items.size(); ++i) {
    if (portfolio_.items[i].symbol == symbol)
      return static_cast<int>(i);
  }
  return -1;
}

}  // namespace hft
