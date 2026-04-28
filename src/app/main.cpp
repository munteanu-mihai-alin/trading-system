#include <filesystem>
#include <iostream>
#include <memory>
#include <vector>

#include "bench/bench.hpp"
#include "broker/IBKRClient.hpp"
#include "broker/PaperBrokerSim.hpp"
#include "config/AppConfig.hpp"
#include "config/LiveTradingConfig.hpp"
#include "engine/LiveExecutionEngine.hpp"
#include "models/symbol_universe.hpp"

#if defined(HFT_ENABLE_STATE_LOGGING)
#include "log/logging_state.hpp"
namespace hl = hft::log;
#endif

int main() {
#if defined(HFT_ENABLE_STATE_LOGGING)
  hl::initialize_logging();
  hl::set_app_state(hl::AppState::Starting);
  hl::set_component_state(hl::ComponentId::Logger, hl::ComponentState::Ready);
#endif

  const std::string config_path = "config.ini";
  std::cout << "Loading config from: "
            << std::filesystem::absolute(config_path).string() << std::endl;
#if defined(HFT_ENABLE_STATE_LOGGING)
  hl::set_app_state(hl::AppState::LoadingConfig);
#endif

  const auto cfg = hft::AppConfig::load_from_file(config_path);
  const auto live_cfg = hft::LiveTradingConfig::from_app(cfg);
  std::cout << "Config loaded. mode=" << live_cfg.mode_name()
            << " steps=" << cfg.steps << " host=" << cfg.host
            << " client_id=" << cfg.client_id << std::endl;

  std::unique_ptr<hft::IBroker> broker;
  hft::IBKRClient* raw_ibkr = nullptr;
  if (live_cfg.use_real_ibkr) {
    std::cout << "Creating real IBKR broker" << std::endl;
    broker = std::make_unique<hft::IBKRClient>();
    raw_ibkr = static_cast<hft::IBKRClient*>(broker.get());
  } else {
    std::cout << "Creating simulated/paper broker" << std::endl;
    broker = std::make_unique<hft::PaperBrokerSim>();
  }
#if defined(HFT_ENABLE_STATE_LOGGING)
  hl::set_component_state(hl::ComponentId::Broker,
                          hl::ComponentState::Starting);
  hl::set_app_state(hl::AppState::ConnectingBroker);
#endif

  hft::LiveExecutionEngine engine(live_cfg, std::move(broker));
  std::cout << "Starting engine..." << std::endl;
  if (!engine.start()) {
    std::cerr << "Failed to connect broker" << std::endl;
#if defined(HFT_ENABLE_STATE_LOGGING)
    hl::set_component_state(hl::ComponentId::Broker, hl::ComponentState::Error);
    hl::set_app_state(hl::AppState::Fatal);
    hl::shutdown_logging();
#endif
    return 1;
  }
  std::cout << "Engine started." << std::endl;
#if defined(HFT_ENABLE_STATE_LOGGING)
  hl::set_component_state(hl::ComponentId::Broker, hl::ComponentState::Ready);
  hl::set_component_state(hl::ComponentId::Engine, hl::ComponentState::Ready);
#endif

  engine.initialize_universe(30);
  std::cout << "Universe initialized." << std::endl;
#if defined(HFT_ENABLE_STATE_LOGGING)
  hl::set_component_state(hl::ComponentId::Universe, hl::ComponentState::Ready);
#endif
  std::vector<std::string> symbols;
  for (const auto& item : hft::kSymbolCompanyList)
    symbols.push_back(item.first);
  std::cout << "Subscribing live books for " << symbols.size() << " symbols..."
            << std::endl;
  engine.subscribe_live_books(symbols);
  std::cout << "Subscriptions requested." << std::endl;
#if defined(HFT_ENABLE_STATE_LOGGING)
  hl::set_component_state(hl::ComponentId::MarketData,
                          hl::ComponentState::Ready);
  hl::set_app_state(hl::AppState::Live);
#endif

  if (raw_ibkr != nullptr) {
    std::cout << "Entering IBKR production event loop. This may block until "
                 "disconnect."
              << std::endl;
    raw_ibkr->start_production_event_loop();
    std::cout << "IBKR production event loop returned." << std::endl;
  }

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
#if defined(HFT_ENABLE_STATE_LOGGING)
  hl::set_app_state(hl::AppState::ShuttingDown);
  hl::shutdown_logging();
#endif
  return 0;
}
