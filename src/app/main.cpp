#include <iostream>
#include <memory>
#include <vector>

#include "bench/bench.hpp"
#include "broker/IBKRClient.hpp"
#include "broker/PaperBrokerSim.hpp"
#include "config/AppConfig.hpp"
#include "config/LiveTradingConfig.hpp"
#include "engine/LiveExecutionEngine.hpp"

int main() {
  const auto cfg = hft::AppConfig::load_from_file("config.ini");
  const auto live_cfg = hft::LiveTradingConfig::from_app(cfg);

  std::unique_ptr<hft::IBroker> broker;
  hft::IBKRClient* raw_ibkr = nullptr;
  if (live_cfg.use_real_ibkr) {
    broker = std::make_unique<hft::IBKRClient>();
    raw_ibkr = static_cast<hft::IBKRClient*>(broker.get());
  } else {
    broker = std::make_unique<hft::PaperBrokerSim>();
  }

  hft::LiveExecutionEngine engine(live_cfg, std::move(broker));
  if (!engine.start()) {
    std::cerr << "Failed to connect broker\n";
    return 1;
  }

  engine.initialize_universe(30);
  std::vector<std::string> symbols;
  for (int i = 0; i < 30; ++i)
    symbols.push_back("STK" + std::to_string(i));
  engine.subscribe_live_books(symbols);
  if (raw_ibkr != nullptr)
    raw_ibkr->start_production_event_loop();
  for (int t = 0; t < cfg.steps; ++t) {
    engine.step(t);
  }
  engine.stop();

  const auto summary = hft::summarize_cycles(engine.ranking.cycle_samples);
  std::cout << summary << "\n";
  std::cout << "Validation: calibration="
            << engine.ranking.validation.calibration_error()
            << " rolling=" << engine.ranking.validation.rolling_error_mean()
            << " ks=" << engine.ranking.validation.ks_statistic() << " alarm="
            << (engine.ranking.validation.degradation_alarm(0.35, 0.35, 0.60)
                    ? "ON"
                    : "OFF")
            << "\n";
  std::cout << "Mode: " << live_cfg.mode_name() << "\n";
  std::cout << "Done. Results in shadow_results.csv\n";
  return 0;
}
