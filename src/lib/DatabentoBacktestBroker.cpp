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
#include <system_error>
#include <utility>
#include <vector>

#include "broker/cache_filename.hpp"
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

struct CacheTsRange {
  std::int64_t start_ns = 0;
  std::int64_t end_ns = 0;
};

// Reads the first and last data row of a dated cache file (either L1 or L2)
// and returns their ts_event in nanoseconds. Returns std::nullopt if the file
// is missing, the header doesn't carry ts_event, or no data rows are present.
[[nodiscard]] std::optional<CacheTsRange> read_cache_ts_range(
    const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in.is_open())
    return std::nullopt;

  std::string header;
  if (!std::getline(in, header))
    return std::nullopt;
  if (header.rfind("ts_event", 0) != 0)
    return std::nullopt;  // legacy schema; treat as no cache for range purposes

  CacheTsRange range{};
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

// Parses one L1 top-of-book row. Supports two schemas:
//   - legacy: step,bid_price,bid_size,ask_price,ask_size            (no ts_event)
//   - dated:  ts_event,step,bid_price,bid_size,ask_price,ask_size   (preferred)
// In legacy rows, ts_event_ns is set to 0 to signal "unknown".
[[nodiscard]] bool parse_top_row(const std::string& line,
                                 std::int64_t& ts_event_ns, int& step,
                                 TopOfBook& top) {
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
      top.bid_price = std::stod(fields[2]);
      top.bid_size = std::stod(fields[3]);
      top.ask_price = std::stod(fields[4]);
      top.ask_size = std::stod(fields[5]);
    } else {
      ts_event_ns = 0;
      step = std::stoi(fields[0]);
      top.bid_price = std::stod(fields[1]);
      top.bid_size = std::stod(fields[2]);
      top.ask_price = std::stod(fields[3]);
      top.ask_size = std::stod(fields[4]);
    }
  } catch (...) {
    return false;
  }
  return true;
}

}  // namespace

bool DatabentoBacktestBroker::cache_covers_window(
    const std::optional<std::pair<std::int64_t, std::int64_t>>& cached_range,
    const std::optional<std::int64_t>& req_start,
    const std::optional<std::int64_t>& req_end) {
  // Both tolerances live here so the broker call sites stay one-liners and
  // future agents see the rationale in one place (and the test fixture
  // covers it).
  constexpr std::int64_t kCacheStartToleranceNs = 60LL * 1'000'000'000LL;
  constexpr std::int64_t kCacheEndToleranceNs =
      24LL * 60 * 60 * 1'000'000'000LL;

  if (!cached_range)
    return false;
  if (req_start && cached_range->first > *req_start + kCacheStartToleranceNs) {
    return false;
  }
  if (req_end && cached_range->second < *req_end - kCacheEndToleranceNs) {
    return false;
  }
  return true;
}

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

  // L1: advance by step index. One L1 row per minute-bar, so step t maps
  // directly to row t. The row's ts_event becomes "wall-clock at engine
  // step t" for that symbol, which the L2 path below uses to time-pace.
  std::unordered_map<std::string, std::int64_t> l1_ts_by_symbol;
  l1_ts_by_symbol.reserve(top_replay_by_ticker_.size());
  for (auto& item : top_replay_by_ticker_) {
    auto& series = item.second;
    if (series.books.empty())
      continue;
    const auto idx = static_cast<std::size_t>(std::min<int>(
        current_step_, static_cast<int>(series.books.size() - 1)));
    series.current = series.books[idx];
    series.current_ts_event =
        (idx < series.ts_events.size()) ? series.ts_events[idx] : 0;
    if (series.current_ts_event > 0)
      l1_ts_by_symbol[series.symbol] = series.current_ts_event;
  }

  // L2: advance the per-series cursor monotonically until the next ts_event
  // would exceed the matching L1 ts_event for the same symbol. This keeps
  // L1 and L2 time-aligned: at engine step t, the L2 book is the latest
  // snapshot whose exchange timestamp is at or before L1's minute-bar t.
  //
  // Legacy fallback (no ts_events in cache, OR no matching L1 series):
  // advance by step index, preserving the prior 1-row-per-step behaviour.
  for (auto& item : replay_by_ticker_) {
    auto& series = item.second;
    if (series.books.empty())
      continue;
    const auto l1_it = l1_ts_by_symbol.find(series.symbol);
    if (series.ts_events.empty() || l1_it == l1_ts_by_symbol.end() ||
        l1_it->second <= 0) {
      const auto idx = static_cast<std::size_t>(std::min<int>(
          current_step_, static_cast<int>(series.books.size() - 1)));
      series.cursor = idx;
      series.current = series.books[idx];
      continue;
    }
    const std::int64_t l1_ts = l1_it->second;
    while (series.cursor + 1 < series.ts_events.size() &&
           series.ts_events[series.cursor + 1] <= l1_ts) {
      ++series.cursor;
    }
    series.current = series.books[series.cursor];
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

std::filesystem::path DatabentoBacktestBroker::new_download_path_for_symbol(
    const std::filesystem::path& root, const std::string& symbol,
    cache::Kind kind, std::int64_t req_start_ns,
    std::int64_t req_end_ns) const {
  return root / cache::format_folder_name(req_start_ns, req_end_ns) /
         cache::format_filename(safe_symbol_filename(symbol), req_start_ns,
                                req_end_ns, kind);
}

std::optional<std::filesystem::path>
DatabentoBacktestBroker::find_covering_file(const std::filesystem::path& root,
                                            const std::string& safe_symbol,
                                            cache::Kind kind,
                                            std::int64_t req_start_ns,
                                            std::int64_t req_end_ns) const {
  std::error_code ec;
  if (!std::filesystem::exists(root, ec) || ec)
    return std::nullopt;

  const std::string prefix = safe_symbol + "_";
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(root, ec)) {
    if (ec)
      break;
    if (!entry.is_regular_file(ec))
      continue;
    const auto name = entry.path().filename().string();
    // Cheap prefix filter to skip files for other symbols before the full
    // parser runs. Embedded underscores in the symbol survive the parse
    // (anchored at the right), but a SAFE_SYM_OTHER_... line would prefix-
    // match here - the post-parse symbol equality check catches that.
    if (name.size() <= prefix.size())
      continue;
    if (name.compare(0, prefix.size(), prefix) != 0)
      continue;

    const auto parsed = cache::parse_filename(name);
    if (!parsed)
      continue;
    if (parsed->kind != kind)
      continue;
    if (parsed->symbol != safe_symbol)
      continue;

    if (cache_covers_window(std::make_optional(std::make_pair(parsed->start_ns,
                                                              parsed->end_ns)),
                            req_start_ns, req_end_ns)) {
      return entry.path();
    }
  }
  return std::nullopt;
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
  const auto req_start = parse_iso8601_to_ns(cfg_.databento_start);
  const auto req_end = parse_iso8601_to_ns(cfg_.databento_end);
  if (!req_start || !req_end)
    return false;  // dated layout needs a window

  const std::filesystem::path root = cfg_.databento_l1_dataset;
  const auto safe = safe_symbol_filename(req.symbol);

  // First try cross-folder cover lookup: any file under root whose filename
  // range covers [req_start, req_end] is reusable as-is. The load step
  // below filters by ts_event to trim a wider file to the requested window.
  auto path =
      find_covering_file(root, safe, cache::Kind::L1, *req_start, *req_end);

  if (!path) {
    // No covering file - download fresh into the dated path. Folder
    // reflects the requested window (not the actual data span), which keeps
    // the path predictable across runs that ask for the same window.
    const auto out = new_download_path_for_symbol(
        root, req.symbol, cache::Kind::L1, *req_start, *req_end);
    std::filesystem::create_directories(out.parent_path());
    std::error_code ec;
    if (std::filesystem::exists(out)) {
      std::filesystem::remove(out, ec);
    }
    const auto cmd = l1_downloader_command(req.symbol, out);
    const int rc = std::system(cmd.c_str());
    if (rc != 0 || !std::filesystem::exists(out))
      return false;
    path = out;
  }

  std::vector<std::int64_t> ts_events;
  auto books = load_top_books_from_csv(*path, req_start, req_end, &ts_events);
  if (books.empty())
    return false;

  TopReplaySeries series;
  series.symbol = req.symbol;
  series.books = std::move(books);
  series.ts_events = std::move(ts_events);
  const auto idx = std::min<std::size_t>(
      static_cast<std::size_t>(current_step_), series.books.size() - 1);
  series.current = series.books[idx];
  series.current_ts_event =
      (idx < series.ts_events.size()) ? series.ts_events[idx] : 0;
  top_replay_by_ticker_[req.ticker_id] = std::move(series);
  return true;
}

bool DatabentoBacktestBroker::ensure_l2_symbol_loaded(
    const MarketDepthRequest& req) {
  const auto req_start = parse_iso8601_to_ns(cfg_.databento_start);
  const auto req_end = parse_iso8601_to_ns(cfg_.databento_end);
  if (!req_start || !req_end)
    return false;  // dated layout needs a window

  const std::filesystem::path root = cfg_.databento_cache_dir;
  const auto safe = safe_symbol_filename(req.symbol);

  auto path =
      find_covering_file(root, safe, cache::Kind::L2, *req_start, *req_end);

  if (!path) {
    const auto out = new_download_path_for_symbol(
        root, req.symbol, cache::Kind::L2, *req_start, *req_end);
    std::filesystem::create_directories(out.parent_path());
    std::error_code ec;
    if (std::filesystem::exists(out)) {
      std::filesystem::remove(out, ec);
    }
    const auto cmd = l2_downloader_command(req.symbol, out, req.depth);
    const int rc = std::system(cmd.c_str());
    if (rc != 0 || !std::filesystem::exists(out))
      return false;
    path = out;
  }

  std::vector<std::int64_t> ts_events;
  auto books = load_books_from_csv(*path, req_start, req_end, &ts_events);
  if (books.empty())
    return false;

  ReplaySeries series;
  series.symbol = req.symbol;
  series.books = std::move(books);
  series.ts_events = std::move(ts_events);
  // Initialise the L2 cursor to the row whose ts_event matches the matching
  // L1 series at current_step_. This matters when L2 is lazy-loaded mid-run
  // (e.g., a position opens at step 4250) so the L2 stream starts at the
  // right wall-clock instead of step 0.
  std::int64_t l1_ts_at_now = 0;
  for (const auto& kv : top_replay_by_ticker_) {
    if (kv.second.symbol == series.symbol) {
      l1_ts_at_now = kv.second.current_ts_event;
      break;
    }
  }
  std::size_t cursor = 0;
  if (l1_ts_at_now > 0 && !series.ts_events.empty()) {
    while (cursor + 1 < series.ts_events.size() &&
           series.ts_events[cursor + 1] <= l1_ts_at_now) {
      ++cursor;
    }
  } else {
    // Legacy fallback: align to current_step_ index.
    cursor = std::min<std::size_t>(static_cast<std::size_t>(current_step_),
                                   series.books.size() - 1);
  }
  series.cursor = cursor;
  series.current = series.books[cursor];
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
    const std::filesystem::path& path, std::optional<std::int64_t> start_ns,
    std::optional<std::int64_t> end_ns,
    std::vector<std::int64_t>* out_ts_events) const {
  std::ifstream in(path);
  if (!in.is_open())
    return {};

  std::vector<TopOfBook> books;
  std::string line;
  // Mirrors the L2 loader's renumber-on-filter behavior. Legacy (no ts_event)
  // rows yield ts_event_ns == 0 and bypass the filter, preserving today's
  // clamp-at-end semantics for any pre-existing legacy L1 cache.
  bool have_step_offset = false;
  int step_offset = 0;
  while (std::getline(in, line)) {
    std::int64_t ts_event_ns = 0;
    int step = 0;
    TopOfBook top;
    if (!parse_top_row(line, ts_event_ns, step, top))
      continue;
    if (step < 0)
      continue;

    if (ts_event_ns > 0) {
      if (start_ns && ts_event_ns < *start_ns)
        continue;
      if (end_ns && ts_event_ns > *end_ns)
        break;
      if (!have_step_offset) {
        step_offset = step;
        have_step_offset = true;
      }
      step -= step_offset;
    }

    const auto idx = static_cast<std::size_t>(step);
    if (idx >= books.size()) {
      books.resize(idx + 1);
      if (out_ts_events)
        out_ts_events->resize(idx + 1, 0);
    }
    books[idx] = top;
    if (out_ts_events)
      (*out_ts_events)[idx] = ts_event_ns;
  }
  return books;
}

std::vector<L2Book> DatabentoBacktestBroker::load_books_from_csv(
    const std::filesystem::path& path, std::optional<std::int64_t> start_ns,
    std::optional<std::int64_t> end_ns,
    std::vector<std::int64_t>* out_ts_events) const {
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
      if (out_ts_events)
        out_ts_events->resize(idx + 1, 0);
    }
    if (out_ts_events && (*out_ts_events)[idx] == 0)
      (*out_ts_events)[idx] = ts_event_ns;

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
