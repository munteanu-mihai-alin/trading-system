#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "broker/IBKRClient.hpp"
#include "log/logging_state.hpp"

namespace {

struct Options {
  std::string host = "127.0.0.1";
  int port = 4002;
  int client_id = 9101;
  std::string symbol = "AAPL";
  bool is_buy = true;
  double qty = 1.0;
  double limit = 0.0;
  int timeout_sec = 45;
  int cancel_after_sec = 20;
  bool transmit = false;
  bool allow_custom_port = false;
};

void print_usage() {
  std::cout
      << "Usage: ibkr_paper_order_probe --limit <price> --transmit [options]\n"
      << "\n"
      << "Submits one tiny limit order to an IBKR paper TWS/Gateway session.\n"
      << "Defaults: --host 127.0.0.1 --port 4002 --client-id 9101 "
      << "--symbol AAPL --action BUY --qty 1 --cancel-after-sec 20\n"
      << "\n"
      << "Safety rails:\n"
      << "  * refuses to submit unless --transmit is provided\n"
      << "  * accepts only paper ports 4002 or 7497 unless "
         "--allow-custom-port is provided\n"
      << "  * refuses quantity above 1 share\n"
      << "\n"
      << "Options:\n"
      << "  --host <host>\n"
      << "  --port <port>                 Paper Gateway default: 4002; paper "
         "TWS default: 7497\n"
      << "  --client-id <id>\n"
      << "  --symbol <symbol>             Stock symbol, routed as "
         "STK/SMART/USD\n"
      << "  --action BUY|SELL\n"
      << "  --qty <shares>                Maximum allowed: 1\n"
      << "  --limit <price>               Required positive limit price\n"
      << "  --timeout-sec <seconds>\n"
      << "  --cancel-after-sec <seconds>  0 disables auto-cancel\n"
      << "  --allow-custom-port\n"
      << "  --transmit\n";
}

bool parse_int(const std::string& text, int& out) {
  char* end = nullptr;
  const long value = std::strtol(text.c_str(), &end, 10);
  if (end == text.c_str() || *end != '\0')
    return false;
  out = static_cast<int>(value);
  return true;
}

bool parse_double(const std::string& text, double& out) {
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || *end != '\0')
    return false;
  out = value;
  return true;
}

bool parse_args(int argc, char** argv, Options& out) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto require_value = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::cerr << name << " requires a value\n";
        return nullptr;
      }
      return argv[++i];
    };

    if (arg == "--help" || arg == "-h") {
      print_usage();
      std::exit(0);
    } else if (arg == "--host") {
      const char* value = require_value("--host");
      if (value == nullptr)
        return false;
      out.host = value;
    } else if (arg == "--port") {
      const char* value = require_value("--port");
      if (value == nullptr || !parse_int(value, out.port))
        return false;
    } else if (arg == "--client-id") {
      const char* value = require_value("--client-id");
      if (value == nullptr || !parse_int(value, out.client_id))
        return false;
    } else if (arg == "--symbol") {
      const char* value = require_value("--symbol");
      if (value == nullptr)
        return false;
      out.symbol = value;
    } else if (arg == "--action") {
      const char* value = require_value("--action");
      if (value == nullptr)
        return false;
      const std::string action = value;
      if (action == "BUY") {
        out.is_buy = true;
      } else if (action == "SELL") {
        out.is_buy = false;
      } else {
        std::cerr << "--action must be BUY or SELL\n";
        return false;
      }
    } else if (arg == "--qty") {
      const char* value = require_value("--qty");
      if (value == nullptr || !parse_double(value, out.qty))
        return false;
    } else if (arg == "--limit") {
      const char* value = require_value("--limit");
      if (value == nullptr || !parse_double(value, out.limit))
        return false;
    } else if (arg == "--timeout-sec") {
      const char* value = require_value("--timeout-sec");
      if (value == nullptr || !parse_int(value, out.timeout_sec))
        return false;
    } else if (arg == "--cancel-after-sec") {
      const char* value = require_value("--cancel-after-sec");
      if (value == nullptr || !parse_int(value, out.cancel_after_sec))
        return false;
    } else if (arg == "--allow-custom-port") {
      out.allow_custom_port = true;
    } else if (arg == "--transmit") {
      out.transmit = true;
    } else {
      std::cerr << "Unknown option: " << arg << "\n";
      return false;
    }
  }
  return true;
}

const char* status_name(hft::OrderLifecycleStatus status) {
  switch (status) {
    case hft::OrderLifecycleStatus::New:
      return "New";
    case hft::OrderLifecycleStatus::Submitted:
      return "Submitted";
    case hft::OrderLifecycleStatus::PartiallyFilled:
      return "PartiallyFilled";
    case hft::OrderLifecycleStatus::Filled:
      return "Filled";
    case hft::OrderLifecycleStatus::Cancelled:
      return "Cancelled";
    case hft::OrderLifecycleStatus::Rejected:
      return "Rejected";
    case hft::OrderLifecycleStatus::Unknown:
      return "Unknown";
  }
  return "Unknown";
}

bool validate_options(const Options& opt) {
  if (!opt.transmit) {
    std::cerr << "Refusing to submit without --transmit.\n";
    return false;
  }
  if (!opt.allow_custom_port && opt.port != 4002 && opt.port != 7497) {
    std::cerr << "Refusing non-paper default port " << opt.port
              << ". Use paper Gateway 4002 or paper TWS 7497, or pass "
                 "--allow-custom-port after verifying the session is paper.\n";
    return false;
  }
  if (opt.qty <= 0.0 || opt.qty > 1.0) {
    std::cerr << "Refusing quantity " << opt.qty
              << ". This probe allows 0 < qty <= 1.\n";
    return false;
  }
  if (opt.limit <= 0.0) {
    std::cerr << "--limit must be a positive price.\n";
    return false;
  }
  if (opt.timeout_sec <= 0) {
    std::cerr << "--timeout-sec must be positive.\n";
    return false;
  }
  if (opt.cancel_after_sec < 0) {
    std::cerr << "--cancel-after-sec cannot be negative.\n";
    return false;
  }
  return true;
}

void print_new_errors(const hft::IBKRClient& client, std::size_t& seen) {
  const auto errors = client.errors();
  while (seen < errors.size()) {
    const auto& err = errors[seen++];
    std::cerr << "IBKR error id=" << err.request_id << " code=" << err.code
              << " message=\"" << err.message << "\"";
    if (!err.advanced_order_reject_json.empty()) {
      std::cerr << " advanced=\"" << err.advanced_order_reject_json << "\"";
    }
    std::cerr << "\n";
  }
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parse_args(argc, argv, opt) || !validate_options(opt)) {
    print_usage();
    return 2;
  }

  hft::log::initialize_logging();

  hft::IBKRClient client;
  std::cout << "Connecting to IBKR paper API at " << opt.host << ":" << opt.port
            << " client_id=" << opt.client_id << "\n";
  if (!client.connect(opt.host, opt.port, opt.client_id)) {
    std::cerr << "Failed to connect to IBKR paper API.\n";
    hft::log::shutdown_logging();
    return 1;
  }

  std::size_t seen_errors = 0;
  const auto wait_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(opt.timeout_sec);
  while (client.next_valid_order_id() <= 0 &&
         std::chrono::steady_clock::now() < wait_deadline) {
    client.pump_once();
    print_new_errors(client, seen_errors);
  }

  const int order_id = client.next_valid_order_id();
  if (order_id <= 0) {
    std::cerr << "Timed out waiting for nextValidId; no order was sent.\n";
    client.disconnect();
    hft::log::shutdown_logging();
    return 1;
  }

  hft::OrderRequest req;
  req.id = order_id;
  req.symbol = opt.symbol;
  req.is_buy = opt.is_buy;
  req.qty = opt.qty;
  req.limit = opt.limit;
  req.transmit = true;

  std::cout << "Submitting paper " << (opt.is_buy ? "BUY" : "SELL") << " "
            << opt.qty << " " << opt.symbol << " LMT " << opt.limit
            << " order_id=" << order_id << "\n";
  client.place_limit_order(req);

  std::string last_status;
  bool cancel_sent = false;
  const auto submit_time = std::chrono::steady_clock::now();
  const auto final_deadline =
      submit_time + std::chrono::seconds(opt.timeout_sec);

  while (std::chrono::steady_clock::now() < final_deadline) {
    client.pump_once();
    print_new_errors(client, seen_errors);

    const auto* state = client.lifecycle().get(order_id);
    if (state != nullptr) {
      const std::string current = status_name(state->status);
      if (current != last_status) {
        last_status = current;
        std::cout << "Order status: " << current
                  << " filled=" << state->filled_qty
                  << " remaining=" << state->remaining_qty
                  << " avg_fill=" << state->avg_fill_price << "\n";
      }

      if (state->status == hft::OrderLifecycleStatus::Filled ||
          state->status == hft::OrderLifecycleStatus::Cancelled ||
          state->status == hft::OrderLifecycleStatus::Rejected) {
        break;
      }
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::steady_clock::now() - submit_time)
                             .count();
    if (!cancel_sent && opt.cancel_after_sec > 0 &&
        elapsed >= opt.cancel_after_sec) {
      std::cout << "Auto-cancelling unfilled paper order after " << elapsed
                << " seconds.\n";
      client.cancel_order(order_id);
      cancel_sent = true;
    }
  }

  const auto* final_state = client.lifecycle().get(order_id);
  if (final_state == nullptr) {
    std::cout << "No orderStatus received before timeout. Check IBKR API "
                 "settings and paper account UI.\n";
  } else {
    std::cout << "Final status: " << status_name(final_state->status)
              << " filled=" << final_state->filled_qty
              << " remaining=" << final_state->remaining_qty
              << " avg_fill=" << final_state->avg_fill_price << "\n";
  }

  client.disconnect();
  hft::log::shutdown_logging();
  return 0;
}
