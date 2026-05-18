#include "broker/DatabentoBacktestBroker.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

#include "log/logging_state.hpp"

namespace hft {

namespace {

[[nodiscard]] std::string shell_quote(const std::string& value) {
  std::string out = "\"";
  for (const char c : value) {
    if (c == '"')
      out += "\\\"";
    else
      out += c;
  }
  out += "\"";
  return out;
}

[[nodiscard]] std::string safe_symbol_filename(const std::string& symbol) {
  std::string out;
  for (const unsigned char c : symbol) {
    if (std::isalnum(c)) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('_');
    }
  }
  if (out.empty())
    out = "symbol";
  return out;
}

// Parses one L2 ladder row. Supports two schemas:
//   - legacy:  step,side,level,price,size            (no ts_event)
//   - dated:   ts_event,step,side,level,price,size   (preferred)
// In legacy rows, ts_event_ns is set to 0 to signal "unknown".
[[nodiscard]] bool parse_level_row(const std::string& line,
                                   std::int64_t& ts_event_ns, int& step,
                                   std::string& side, int& level, double& price,
                                   double& size) {
  std::stringstream ss(line);
  std::string field;
  std::vector<std::string> fields;
  while (std::getline(ss, field, ',')) {
    fields.push_back(field);
  }
  if (fields.size() < 5)
    return false;
  if (fields[0] == "step" || fields[0] == "ts_event")
    return false;

  try {
    if (fields.size() >= 6) {
      ts_event_ns = std::stoll(fields[0]);
      step = std::stoi(fields[1]);
      side = fields[2];
      level = std::stoi(fields[3]);
      price = std::stod(fields[4]);
      size = std::stod(fields[5]);
    } else {
      ts_event_ns = 0;
      step = std::stoi(fields[0]);
      side = fields[1];
      level = std::stoi(fields[2]);
      price = std::stod(fields[3]);
      size = std::stod(fields[4]);
    }
  } catch (...) {
    return false;
  }
  return true;
}

// Parses an ISO-8601 timestamp like 2026-04-13T13:30:00Z into nanoseconds
// since the Unix epoch. Returns std::nullopt on malformed input. Empty input
// also returns nullopt - callers should treat that as "no bound".
[[nodiscard]] std::optional<std::int64_t> parse_iso8601_to_ns(
    const std::string& iso) {
  if (iso.empty())
    return std::nullopt;
  std::tm tm{};
  int year = 0, mon = 0, day = 0, hour = 0, minute = 0, second = 0;
  // Tolerate trailing "Z" or "+00:00"; we treat all inputs as UTC.
  const int matched = std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &year, &mon,
                                  &day, &hour, &minute, &second);
  if (matched < 6)
    return std::nullopt;
  tm.tm_year = year - 1900;
  tm.tm_mon = mon - 1;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min = minute;
  tm.tm_sec = second;
#if defined(_WIN32)
  const std::time_t t = _mkgmtime(&tm);
#else
  const std::time_t t = timegm(&tm);
#endif
  if (t == static_cast<std::time_t>(-1))
    return std::nullopt;
  return static_cast<std::int64_t>(t) * 1'000'000'000LL;
}

struct L2CacheRange {
  std::int64_t start_ns = 0;
  std::int64_t end_ns = 0;
};

// Reads the first and last data row of an L2 cache file and returns their
// ts_event in nanoseconds. Returns std::nullopt if the file is missing, the
// header doesn't carry ts_event, or no data rows are present.
[[nodiscard]] std::optional<L2CacheRange> read_l2_cache_range(
    const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in.is_open())
    return std::nullopt;

  std::string header;
  if (!std::getline(in, header))
    return std::nullopt;
  if (header.rfind("ts_event", 0) != 0)
    return std::nullopt;  // legacy schema; treat as no cache for range purposes

  L2CacheRange range{};
  bool have_start = false;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty())
      continue;
    const auto comma = line.find(',');
    if (comma == std::string::npos)
      continue;
    std::int64_t ts = 0;
    try {
      ts = std::stoll(line.substr(0, comma));
    } catch (...) {
      continue;
    }
    if (!have_start) {
      range.start_ns = ts;
      have_start = true;
    }
    range.end_ns = ts;
  }
  if (!have_start)
    return std::nullopt;
  return range;
}

[[nodiscard]] bool parse_top_row(const std::string& line, int& step,
                                 TopOfBook& top) {
  std::stringstream ss(line);
  std::string field;
  std::vector<std::string> fields;
  while (std::getline(ss, field, ',')) {
    fields.push_back(field);
  }
  if (fields.size() < 5)
    return false;
  if (fields[0] == "step")
    return false;

  try {
    step = std::stoi(fields[0]);
    top.bid_price = std::stod(fields[1]);
    top.bid_size = std::stod(fields[2]);
    top.ask_price = std::stod(fields[3]);
    top.ask_size = std::stod(fields[4]);
  } catch (...) {
    return false;
  }
  return true;
}

}  // namespace

DatabentoBacktestBroker::DatabentoBacktestBroker(AppConfig cfg)
    : cfg_(std::move(cfg)) {}

bool DatabentoBacktestBroker::connect(const std::string&, int, int) {
  connected_ = true;
  hft::log::set_component_state(hft::log::ComponentId::Broker,
                                hft::log::ComponentState::Ready);
  return true;
}

void DatabentoBacktestBroker::disconnect() {
  if (connected_) {
    hft::log::set_component_state(hft::log::ComponentId::Broker,
                                  hft::log::ComponentState::Down);
  }
  connected_ = false;
}

bool DatabentoBacktestBroker::is_connected() const {
  return connected_;
}

void DatabentoBacktestBroker::place_limit_order(const OrderRequest& req) {
  lifecycle_.on_submitted(req.id, req.symbol, req.qty);
  working_orders_[req.id] = req;
  fill_crossed_orders();
}

void DatabentoBacktestBroker::cancel_order(int order_id) {
  working_orders_.erase(order_id);
  lifecycle_.on_status(order_id, "Cancelled", 0.0, 0.0, 0.0);
}

void DatabentoBacktestBroker::start_event_loop() {}

void DatabentoBacktestBroker::stop_event_loop() {}

void DatabentoBacktestBroker::subscribe_top_of_book(
    const TopOfBookRequest& req) {
  ensure_l1_symbol_loaded(req);
}

void DatabentoBacktestBroker::subscribe_market_depth(
    const MarketDepthRequest& req) {
  ensure_l2_symbol_loaded(req);
}

void DatabentoBacktestBroker::on_step(int t) {
  current_step_ = std::max(t, 0);
  for (auto& item : top_replay_by_ticker_) {
    auto& series = item.second;
    if (series.books.empty())
      continue;
    const auto idx = static_cast<std::size_t>(std::min<int>(
        current_step_, static_cast<int>(series.books.size() - 1)));
    series.current = series.books[idx];
  }
  for (auto& item : replay_by_ticker_) {
    auto& series = item.second;
    if (series.books.empty())
      continue;
    const auto idx = static_cast<std::size_t>(std::min<int>(
        current_step_, static_cast<int>(series.books.size() - 1)));
    series.current = series.books[idx];
  }
  fill_crossed_orders();
}

TopOfBook DatabentoBacktestBroker::snapshot_top_of_book(int ticker_id) const {
  const auto it = top_replay_by_ticker_.find(ticker_id);
  if (it != top_replay_by_ticker_.end() && it->second.current.valid()) {
    return it->second.current;
  }
  // Fallback: derive top-of-book from the L2 series if it is loaded. Lets
  // the backtest run with L2-only data for held symbols, without needing
  // an L1 CSV for every universe member.
  const auto l2_it = replay_by_ticker_.find(ticker_id);
  if (l2_it != replay_by_ticker_.end()) {
    const auto& book = l2_it->second.current;
    if (book.best_bid() > 0.0 && book.best_ask() > 0.0 &&
        book.best_bid() <= book.best_ask()) {
      return TopOfBook{book.best_bid(), book.bids[0].size, book.best_ask(),
                       book.asks[0].size};
    }
  }
  return {};
}

L2Book DatabentoBacktestBroker::snapshot_book(int ticker_id) const {
  const auto it = replay_by_ticker_.find(ticker_id);
  if (it == replay_by_ticker_.end())
    return {};
  return it->second.current;
}

const OrderLifecycleBook* DatabentoBacktestBroker::order_lifecycle() const {
  return &lifecycle_;
}

int DatabentoBacktestBroker::max_replay_steps() const {
  // Maximum step index for which we have any L1 or L2 data loaded. The
  // engine should run for that many steps and no more; past this the
  // current-step bounds-check inside on_step() freezes both streams at
  // their last row, and ranking would be making decisions on stale
  // prices.
  std::size_t out = 0;
  for (const auto& kv : top_replay_by_ticker_) {
    out = std::max(out, kv.second.books.size());
  }
  for (const auto& kv : replay_by_ticker_) {
    out = std::max(out, kv.second.books.size());
  }
  return static_cast<int>(out);
}

std::filesystem::path DatabentoBacktestBroker::l1_cache_path_for_symbol(
    const std::string& symbol) const {
  return std::filesystem::path(cfg_.databento_cache_dir) /
         (safe_symbol_filename(symbol) + ".mbp1.csv");
}

std::filesystem::path DatabentoBacktestBroker::l2_cache_path_for_symbol(
    const std::string& symbol) const {
  return std::filesystem::path(cfg_.databento_cache_dir) /
         (safe_symbol_filename(symbol) + ".mbp10.csv");
}

std::string DatabentoBacktestBroker::l1_downloader_command(
    const std::string& symbol, const std::filesystem::path& out) const {
  std::string cmd = shell_quote(cfg_.databento_python) + " " +
                    shell_quote(cfg_.databento_l1_download_script) +
                    " --symbol " + shell_quote(symbol) + " --dataset " +
                    shell_quote(cfg_.databento_l1_dataset) + " --schema " +
                    shell_quote(cfg_.databento_l1_schema) + " --output " +
                    shell_quote(out.string());
  if (!cfg_.databento_start.empty()) {
    cmd += " --start " + shell_quote(cfg_.databento_start);
  }
  if (!cfg_.databento_end.empty()) {
    cmd += " --end " + shell_quote(cfg_.databento_end);
  }
  return cmd;
}

std::string DatabentoBacktestBroker::l2_downloader_command(
    const std::string& symbol, const std::filesystem::path& out,
    int depth) const {
  std::string cmd = shell_quote(cfg_.databento_python) + " " +
                    shell_quote(cfg_.databento_l2_download_script) +
                    " --symbol " + shell_quote(symbol) + " --dataset " +
                    shell_quote(cfg_.databento_l2_dataset) + " --schema " +
                    shell_quote(cfg_.databento_l2_schema) + " --output " +
                    shell_quote(out.string()) + " --depth " +
                    std::to_string(std::max(depth, 1));
  if (!cfg_.databento_start.empty()) {
    cmd += " --start " + shell_quote(cfg_.databento_start);
  }
  if (!cfg_.databento_end.empty()) {
    cmd += " --end " + shell_quote(cfg_.databento_end);
  }
  return cmd;
}

bool DatabentoBacktestBroker::ensure_l1_symbol_loaded(
    const TopOfBookRequest& req) {
  const auto out = l1_cache_path_for_symbol(req.symbol);
  if (!download_if_missing(out, l1_downloader_command(req.symbol, out)))
    return false;

  auto books = load_top_books_from_csv(out);
  if (books.empty())
    return false;

  TopReplaySeries series;
  series.symbol = req.symbol;
  series.books = std::move(books);
  series.current = series.books[std::min<std::size_t>(
      static_cast<std::size_t>(current_step_), series.books.size() - 1)];
  top_replay_by_ticker_[req.ticker_id] = std::move(series);
  return true;
}

bool DatabentoBacktestBroker::ensure_l2_symbol_loaded(
    const MarketDepthRequest& req) {
  const auto out = l2_cache_path_for_symbol(req.symbol);
  const auto req_start = parse_iso8601_to_ns(cfg_.databento_start);
  const auto req_end = parse_iso8601_to_ns(cfg_.databento_end);

  bool need_download = !std::filesystem::exists(out);
  if (!need_download) {
    // Only consider the cache reusable when both the cached file carries a
    // ts_event range AND the requested window fits inside it. Either side
    // missing -> re-download to guarantee the replay covers the request.
    const auto cached = read_l2_cache_range(out);
    if (!cached) {
      need_download = true;  // legacy schema or empty
    } else if (req_start && cached->start_ns > *req_start) {
      need_download = true;
    } else if (req_end && cached->end_ns < *req_end) {
      need_download = true;
    }
  }

  if (need_download) {
    std::filesystem::create_directories(out.parent_path());
    if (std::filesystem::exists(out)) {
      std::error_code ec;
      std::filesystem::remove(out, ec);
    }
    const auto cmd = l2_downloader_command(req.symbol, out, req.depth);
    const int rc = std::system(cmd.c_str());
    if (rc != 0 || !std::filesystem::exists(out))
      return false;
  }

  auto books = load_books_from_csv(out, req_start, req_end);
  if (books.empty())
    return false;

  ReplaySeries series;
  series.symbol = req.symbol;
  series.books = std::move(books);
  series.current = series.books[std::min<std::size_t>(
      static_cast<std::size_t>(current_step_), series.books.size() - 1)];
  replay_by_ticker_[req.ticker_id] = std::move(series);
  return true;
}

bool DatabentoBacktestBroker::download_if_missing(
    const std::filesystem::path& out, const std::string& command) const {
  if (std::filesystem::exists(out))
    return true;

  std::filesystem::create_directories(out.parent_path());
  const int rc = std::system(command.c_str());
  return rc == 0 && std::filesystem::exists(out);
}

std::vector<TopOfBook> DatabentoBacktestBroker::load_top_books_from_csv(
    const std::filesystem::path& path) const {
  std::ifstream in(path);
  if (!in.is_open())
    return {};

  std::vector<TopOfBook> books;
  std::string line;
  while (std::getline(in, line)) {
    int step = 0;
    TopOfBook top;
    if (!parse_top_row(line, step, top))
      continue;
    if (step < 0)
      continue;

    const auto idx = static_cast<std::size_t>(step);
    if (idx >= books.size()) {
      books.resize(idx + 1);
    }
    books[idx] = top;
  }
  return books;
}

std::vector<L2Book> DatabentoBacktestBroker::load_books_from_csv(
    const std::filesystem::path& path, std::optional<std::int64_t> start_ns,
    std::optional<std::int64_t> end_ns) const {
  std::ifstream in(path);
  if (!in.is_open())
    return {};

  std::vector<L2Book> books;
  std::string line;
  // For dated caches we renumber step to 0..N over the kept rows by remembering
  // the first surviving original step and subtracting. Legacy (no ts_event)
  // files have ts_event_ns == 0 and bypass the filter, preserving today's
  // behavior for any pre-existing cached file.
  bool have_step_offset = false;
  int step_offset = 0;
  while (std::getline(in, line)) {
    std::int64_t ts_event_ns = 0;
    int step = 0;
    int level = 0;
    double price = 0.0;
    double size = 0.0;
    std::string side;
    if (!parse_level_row(line, ts_event_ns, step, side, level, price, size))
      continue;
    if (step < 0 || level < 0 || level >= L2Book::DEPTH)
      continue;

    // Apply ts_event window only when the row carries a timestamp AND the
    // caller requested a bound. ts_event_ns == 0 means legacy schema; we
    // keep every row in that case.
    if (ts_event_ns > 0) {
      if (start_ns && ts_event_ns < *start_ns)
        continue;
      if (end_ns && ts_event_ns > *end_ns)
        break;  // CSV is monotonic in ts_event
      if (!have_step_offset) {
        step_offset = step;
        have_step_offset = true;
      }
      step -= step_offset;
    }

    const auto idx = static_cast<std::size_t>(step);
    if (idx >= books.size()) {
      books.resize(idx + 1);
    }

    L2Level l{price, size};
    if (side == "bid" || side == "B" || side == "b" || side == "0") {
      books[idx].bids[static_cast<std::size_t>(level)] = l;
    } else if (side == "ask" || side == "A" || side == "a" || side == "1") {
      books[idx].asks[static_cast<std::size_t>(level)] = l;
    }
  }
  return books;
}

TopOfBook DatabentoBacktestBroker::top_for_symbol(
    const std::string& symbol) const {
  for (const auto& item : top_replay_by_ticker_) {
    if (item.second.symbol == symbol) {
      return item.second.current;
    }
  }

  const auto depth = depth_for_symbol(symbol);
  if (depth.best_bid() <= 0.0 || depth.best_ask() <= 0.0)
    return {};
  return TopOfBook{depth.best_bid(), depth.bids[0].size, depth.best_ask(),
                   depth.asks[0].size};
}

L2Book DatabentoBacktestBroker::depth_for_symbol(
    const std::string& symbol) const {
  for (const auto& item : replay_by_ticker_) {
    if (item.second.symbol == symbol) {
      return item.second.current;
    }
  }
  return {};
}

void DatabentoBacktestBroker::fill_crossed_orders() {
  for (auto it = working_orders_.begin(); it != working_orders_.end();) {
    const auto& req = it->second;
    const auto book = depth_for_symbol(req.symbol);
    const auto top = top_for_symbol(req.symbol);

    double fill_price = 0.0;
    const double best_ask =
        (book.best_ask() > 0.0) ? book.best_ask() : top.ask_price;
    const double best_bid =
        (book.best_bid() > 0.0) ? book.best_bid() : top.bid_price;
    if (req.is_buy && best_ask > 0.0 && req.limit >= best_ask) {
      fill_price = best_ask;
    } else if (!req.is_buy && best_bid > 0.0 && req.limit <= best_bid) {
      fill_price = best_bid;
    }

    if (fill_price > 0.0) {
      lifecycle_.on_status(req.id, "Filled", req.qty, 0.0, fill_price);
      it = working_orders_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace hft
