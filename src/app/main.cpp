#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "app/build_info.hpp"
#include "app/effective_steps.hpp"
#include "app/step_pacing.hpp"
#include "broker/DatabentoBacktestBroker.hpp"
#include "broker/IBKRClient.hpp"
#include "broker/LocalSimBroker.hpp"
#include "config/AppConfig.hpp"
#include "config/LiveTradingConfig.hpp"
#include "engine/Chronos2ExecutionEngine.hpp"
#include "log/logging_state.hpp"
#include "models/symbol_universe.hpp"

namespace hl = hft::log;

// Chronos-MR-PredExit is the only strategy on this branch: rank by
// Chronos's predicted return, exit at the predicted price (reference:
// research/quantconnect/Chronos_MR_PredExit.py). The former hawkes/OU
// LiveExecutionEngine + RankingEngine path was removed.
int main(int argc, char** argv) {
  // ---- Build-provenance query mode ----
  //
  // Any of --branch / --commit / --version prints what was asked for
  // and exits WITHOUT trading. Deliberate: a provenance query must
  // never be able to start a live session by accident, so this gate
  // runs before logging, config load, or broker connect.
  //
  // Output is script-friendly: one flag prints the bare value, several
  // print labeled key=value lines.
  //
  // An unrecognized argument is a hard error (exit 2) rather than being
  // ignored. hft_app.service passes no arguments at all, so any argv we
  // do not understand means the invocation is not what someone thought
  // it was -- refusing to trade is the safe response.
  if (argc > 1) {
    bool want_branch = false;
    bool want_commit = false;
    bool want_version = false;
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--branch") {
        want_branch = true;
      } else if (arg == "--commit") {
        want_commit = true;
      } else if (arg == "--version") {
        want_version = true;
      } else {
        std::cerr << "hft_app: unrecognized argument: " << arg << "\n"
                  << "usage: hft_app [--branch] [--commit] [--version]\n"
                  << "  no arguments: run the trading engine\n";
        return 2;
      }
    }
    const int n =
        (want_branch ? 1 : 0) + (want_commit ? 1 : 0) + (want_version ? 1 : 0);
    const bool label = n > 1;
    if (want_version) {
      std::cout << (label ? "version=" : "") << hft::build_info::version()
                << std::endl;
    }
    if (want_branch) {
      std::cout << (label ? "branch=" : "") << hft::build_info::branch()
                << std::endl;
    }
    if (want_commit) {
      std::cout << (label ? "commit=" : "") << hft::build_info::commit()
                << std::endl;
    }
    return 0;
  }

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
  } else if (cfg.mode == hft::BrokerMode::DatabentoBacktest) {
    std::cout << "Creating Databento backtest broker" << std::endl;
    broker = std::make_unique<hft::DatabentoBacktestBroker>(cfg);
  } else {
    std::cout << "Creating local simulated broker" << std::endl;
    broker = std::make_unique<hft::LocalSimBroker>();
  }
  hl::set_app_state(hl::AppState::ConnectingBroker);

  hft::Chronos2ExecutionEngine engine(live_cfg, std::move(broker));
  std::cout << "Starting Chronos engine..." << std::endl;
  if (!engine.start()) {
    std::cerr << "Failed to start Chronos engine" << std::endl;
    hl::set_app_state(hl::AppState::Fatal);
    hl::shutdown_logging();
    return 1;
  }

  // IBKR live/paper: start the production reader loop, wait for the
  // broker to hand us a nextValidId, then seed the engine order-id
  // counter from it. IBKR rejects ids below nextValidId.
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
    engine.sync_next_order_id_from_broker();
    std::cout << "IBKR nextValidId=" << raw_ibkr->next_valid_order_id()
              << std::endl;
  }

  const int uni_size = std::clamp(
      cfg.universe_size, 0, static_cast<int>(hft::kSymbolCompanyList.size()));
  engine.initialize_universe(hft::kSymbolCompanyList, uni_size);
  engine.subscribe_live_books();

  hl::set_app_state(hl::AppState::Live);
  const int steps = hft::compute_effective_steps(
      cfg.steps, cfg.steps_auto_from_broker, cfg.mode, 0);
  std::cout << "Running " << steps << " Chronos engine steps..." << std::endl;
  // Paced on an absolute schedule rather than sleep-after-work, so
  // the cadence does not drift by however long each step took.
  const int interval_ms =
      hft::effective_step_interval_ms(cfg.step_interval_ms, cfg.mode);
  if (interval_ms > 0) {
    std::cout << "Pacing steps at " << interval_ms << " ms" << std::endl;
  }
  const auto period = std::chrono::milliseconds(interval_ms);
  auto next_tick = std::chrono::steady_clock::now();
  for (int t = 0; t < steps; ++t) {
    engine.step(t);
    if (interval_ms > 0) {
      next_tick += period;
      const auto now = std::chrono::steady_clock::now();
      if (now < next_tick) {
        std::this_thread::sleep_until(next_tick);
      } else {
        // A step overran its slot (forecast subprocess, GC pause,
        // slow broker call). Resynchronise instead of trying to
        // catch up, which would spin to close a gap that only grows.
        next_tick = now;
      }
    }
  }
  engine.stop();
  std::cout << "Chronos engine stopped. realized_pnl=" << engine.realized_pnl()
            << " bonus_budget=" << engine.bonus_budget()
            << " open_positions=" << engine.open_positions().size()
            << std::endl;

  hl::set_app_state(hl::AppState::ShuttingDown);
  hl::shutdown_logging();
  return 0;
}
