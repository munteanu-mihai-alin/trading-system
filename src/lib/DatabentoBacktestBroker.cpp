#include "broker/DatabentoBacktestBroker.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
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

[[nodiscard]] bool parse_level_row(const std::string& line, int& step,
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
  if (fields[0] == "step")
    return false;

  try {
    step = std::stoi(fields[0]);
    side = fields[1];
    level = std::stoi(fields[2]);
    price = std::stod(fields[3]);
    size = std::stod(fields[4]);
  } catch (...) {
    return false;
  }
  return true;
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
  if (it == top_replay_by_ticker_.end())
    return {};
  return it->second.current;
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
  if (!download_if_missing(out,
                           l2_downloader_command(req.symbol, out, req.depth)))
    return false;

  auto books = load_books_from_csv(out);
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
    const std::filesystem::path& path) const {
  std::ifstream in(path);
  if (!in.is_open())
    return {};

  std::vector<L2Book> books;
  std::string line;
  while (std::getline(in, line)) {
    int step = 0;
    int level = 0;
    double price = 0.0;
    double size = 0.0;
    std::string side;
    if (!parse_level_row(line, step, side, level, price, size))
      continue;
    if (step < 0 || level < 0 || level >= L2Book::DEPTH)
      continue;

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
