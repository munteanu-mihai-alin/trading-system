#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "bench/bench.hpp"
#include "broker/IBKRClient.hpp"
#include "broker/LocalSimBroker.hpp"
#include "config/AppConfig.hpp"
#include "config/LiveTradingConfig.hpp"
#include "engine/LiveExecutionEngine.hpp"
#include "log/logging_state.hpp"
#include "models/symbol_universe.hpp"

namespace hl = hft::log;

int main() {
  hl::initialize_logging();
  hl::set_app_state(hl::AppState::Starting);
  hl::set_component_state(hl::ComponentId::Logger, hl::ComponentState::Ready);

  const std::string config_path = "config.ini";
  std::cout << "Loading config from: "
            << std::filesystem::absolute(config_path).string() << std::endl;
  hl::set_app_state(hl::AppState::LoadingConfig);

  const auto cfg = hft::AppConfig::load_from_file(config_path);
  const auto live_cfg = hft::LiveTradingConfig::from_app(cfg);
  std::cout << "Config loaded. mode=" << live_cfg.mode_name()
            << " steps=" << cfg.steps << " universe_size=" << cfg.universe_size
            << " host=" << cfg.host << " client_id=" << cfg.client_id
            << std::endl;

  std::unique_ptr<hft::IBroker> broker;
  hft::IBKRClient* raw_ibkr = nullptr;
  if (live_cfg.use_real_ibkr) {
    std::cout << "Creating real IBKR broker for " << live_cfg.mode_name()
              << " mode" << std::endl;
    broker = std::make_unique<hft::IBKRClient>();
    raw_ibkr = static_cast<hft::IBKRClient*>(broker.get());
  } else {
    std::cout << "Creating local simulated broker" << std::endl;
    broker = std::make_unique<hft::LocalSimBroker>();
  }
  hl::set_app_state(hl::AppState::ConnectingBroker);

  hft::LiveExecutionEngine engine(live_cfg, std::move(broker));
  std::cout << "Starting engine..." << std::endl;
  if (!engine.start()) {
    std::cerr << "Failed to connect broker" << std::endl;
    hl::set_app_state(hl::AppState::Fatal);
    hl::shutdown_logging();
    return 1;
  }
  std::cout << "Engine started." << std::endl;

  if (raw_ibkr != nullptr) {
    std::cout << "Starting IBKR reader loop." << std::endl;
    raw_ibkr->start_production_event_loop();

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (raw_ibkr->next_valid_order_id() <= 0 &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (raw_ibkr->next_valid_order_id() <= 0) {
      std::cerr << "IBKR did not provide nextValidId before timeout"
                << std::endl;
      engine.stop();
      hl::set_app_state(hl::AppState::Fatal);
      hl::shutdown_logging();
      return 1;
    }
    std::cout << "IBKR nextValidId=" << raw_ibkr->next_valid_order_id()
              << std::endl;
  }

  const int universe_size = std::clamp(
      cfg.universe_size, 0, static_cast<int>(hft::kSymbolCompanyList.size()));
  engine.initialize_universe(universe_size);
  std::cout << "Universe initialized with " << universe_size << " symbols."
            << std::endl;
  std::vector<std::string> symbols;
  for (int i = 0; i < universe_size; ++i) {
    symbols.push_back(
        hft::kSymbolCompanyList[static_cast<std::size_t>(i)].first);
  }
  std::cout << "Subscribing live books for " << symbols.size() << " symbols..."
            << std::endl;
  engine.subscribe_live_books(symbols);
  std::cout << "Subscriptions requested." << std::endl;
  hl::set_app_state(hl::AppState::Live);

  std::cout << "Running " << cfg.steps << " engine steps..." << std::endl;
  for (int t = 0; t < cfg.steps; ++t) {
    engine.step(t);
  }
  engine.stop();
  std::cout << "Engine stopped." << std::endl;

  const auto summary = hft::summarize_cycles(engine.ranking.cycle_samples);
  std::cout << summary << std::endl;
  std::cout << "Validation: calibration="
            << engine.ranking.validation.calibration_error()
            << " rolling=" << engine.ranking.validation.rolling_error_mean()
            << " ks=" << engine.ranking.validation.ks_statistic() << " alarm="
            << (engine.ranking.validation.degradation_alarm(0.35, 0.35, 0.60)
                    ? "ON"
                    : "OFF")
            << std::endl;
  std::cout << "Mode: " << live_cfg.mode_name() << std::endl;
  std::cout << "Done. Results in shadow_results.csv" << std::endl;
  hl::set_app_state(hl::AppState::ShuttingDown);
  hl::shutdown_logging();
  return 0;
}
