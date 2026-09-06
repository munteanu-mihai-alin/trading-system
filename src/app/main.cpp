#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "app/effective_steps.hpp"
#include "bench/bench.hpp"
#include "broker/DatabentoBacktestBroker.hpp"
#include "broker/IBKRClient.hpp"
#include "broker/LocalSimBroker.hpp"
#include "config/AppConfig.hpp"
#include "config/LiveTradingConfig.hpp"
#include "engine/Chronos2ExecutionEngine.hpp"
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
            << " strategy_mode=" << cfg.strategy_mode << std::endl;

  // ---- Strategy dispatch: fork the routing entirely per user config ----
  // "chronos2_mr_pred_exit" runs a separate engine (Chronos2ExecutionEngine).
  // Default "hawkes_ou_mr" falls through to the existing LiveExecutionEngine
  // path below.
  if (cfg.strategy_mode == "chronos2_mr_pred_exit") {
    std::cout << "Dispatching to Chronos2ExecutionEngine "
                 "(chronos2_mr_pred_exit strategy)"
              << std::endl;

    std::unique_ptr<hft::IBroker> broker2;
    if (cfg.mode == hft::BrokerMode::DatabentoBacktest) {
      std::cout << "Creating Databento backtest broker" << std::endl;
      broker2 = std::make_unique<hft::DatabentoBacktestBroker>(cfg);
    } else {
      std::cout << "Creating local simulated broker" << std::endl;
      broker2 = std::make_unique<hft::LocalSimBroker>();
    }

    hft::Chronos2ExecutionEngine engine2(live_cfg, std::move(broker2));
    std::cout << "Starting Chronos2 engine..." << std::endl;
    if (!engine2.start()) {
      std::cerr << "Failed to start Chronos2 engine" << std::endl;
      hl::set_app_state(hl::AppState::Fatal);
      hl::shutdown_logging();
      return 1;
    }

    // Universe: same source rules as the default engine (hard-coded
    // yen-window list; file-based override via symbol_universe_path
    // is TODO for the Chronos2 path).
    const int uni_size = std::clamp(
        cfg.universe_size, 0, static_cast<int>(hft::kSymbolCompanyList.size()));
    engine2.initialize_universe(hft::kSymbolCompanyList, uni_size);
    engine2.subscribe_live_books();

    hl::set_app_state(hl::AppState::Live);
    std::cout << "Running " << cfg.steps << " Chronos2 engine steps..."
              << std::endl;
    for (int t = 0; t < cfg.steps; ++t) {
      engine2.step(t);
    }
    engine2.stop();
    std::cout << "Chronos2 engine stopped. realized_pnl="
              << engine2.realized_pnl()
              << " bonus_budget=" << engine2.bonus_budget()
              << " open_positions=" << engine2.open_positions().size()
              << std::endl;

    hl::set_app_state(hl::AppState::ShuttingDown);
    hl::shutdown_logging();
    return 0;
  }
  // else: fall through to the default LiveExecutionEngine path below.

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
  // The engine resolved the universe internally (file override via
  // cfg.symbol_universe_path, or the hard-coded kSymbolCompanyList as
  // a fallback) and pushed Stock items into ranking.portfolio.items
  // in load order. We MUST derive the subscription list from those
  // items: subscribe_live_books assigns ticker_id = i+1 to the i-th
  // symbol it sees, and reconcile_broker_state later looks up that
  // ticker_id by items[i]. If we build `symbols` from a different
  // source (e.g. the hard-coded list while the file override is set),
  // ticker_id i+1 ends up subscribed for a DIFFERENT symbol than
  // items[i], and every snapshot_top_of_book call returns the wrong
  // L1 file's prices. That is the bug that produced the 2026-05-29
  // yen v3 misroute (KEYS reading IBM's L1, KLAC reading KEYS's L1,
  // every subsequent step seeing wrong mids); see
  // agent/AGENT_HANDOFF_LOG.md.
  std::vector<std::string> symbols;
  symbols.reserve(engine.ranking.portfolio.items.size());
  for (const auto& item : engine.ranking.portfolio.items) {
    symbols.push_back(item.symbol);
  }
  std::cout << "Universe initialized with " << symbols.size() << " symbols."
            << std::endl;
  std::cout << "Subscribing live books for " << symbols.size() << " symbols..."
            << std::endl;
  engine.subscribe_live_books(symbols);
  std::cout << "Subscriptions requested." << std::endl;
  hl::set_app_state(hl::AppState::Live);

  // Step-count override: when steps_auto_from_broker is set and the
  // broker reports a positive max_replay_steps(), the loop runs for
  // min(cfg.steps, broker_max) so we (a) never run past the L1/L2
  // data window, and (b) NEVER run longer than the configured
  // ceiling. (b) was missing pre-fix and caused the 2026-05-29 yen
  // v2 disk-full crash; see include/app/effective_steps.hpp. We peek
  // at the broker via a freshly-constructed throwaway only when
  // mode=databento_backtest because IBKR brokers always return 0.
  int broker_max = 0;
  if (cfg.steps_auto_from_broker &&
      cfg.mode == hft::BrokerMode::DatabentoBacktest) {
    // The owned broker has moved into the engine; the engine doesn't
    // expose a generic broker accessor. Reconstruct a parallel broker
    // to query L1 series length: subscribe_top_of_book for each symbol
    // (reads the same cached CSVs), then max_replay_steps().
    hft::DatabentoBacktestBroker probe(cfg);
    probe.connect("", 0, 0);
    int t = 1;
    for (const auto& sym : symbols) {
      probe.subscribe_top_of_book(hft::TopOfBookRequest{t++, sym});
    }
    broker_max = probe.max_replay_steps();
    probe.disconnect();
  }
  const int effective_steps = hft::compute_effective_steps(
      cfg.steps, cfg.steps_auto_from_broker, cfg.mode, broker_max);
  if (broker_max > 0) {
    std::cout << "steps_auto_from_broker=true: cfg.steps=" << cfg.steps
              << " broker_max=" << broker_max
              << " -> effective_steps=" << effective_steps << std::endl;
  }
  std::cout << "Running " << effective_steps << " engine steps..." << std::endl;
  for (int t = 0; t < effective_steps; ++t) {
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

  // End-of-run trading summary. We don't expose the broker pointer back
  // through the engine (would leak ownership across the seam), so the
  // lifecycle-level fill/PnL breakdown is left to a follow-up. For now we
  // dump what the engine already exposes: order count, open positions,
  // and the per-position state. That's enough to answer "did anything
  // actually trade?" on a backtest run.
  std::cout << "Orders placed (engine): " << engine.orders_placed()
            << std::endl;
  std::cout << "Open positions at end: " << engine.open_positions().size()
            << std::endl;
  double open_notional = 0.0;
  for (const auto& kv : engine.open_positions()) {
    const auto& p = kv.second;
    open_notional += p.qty * p.entry_price;
    std::cout << "  " << p.symbol << " qty=" << p.qty
              << " entry=" << p.entry_price << " sell_limit=" << p.sell_limit
              << " sell_score=" << p.sell_score << std::endl;
  }
  std::cout << "Open notional at end: " << open_notional << std::endl;

  std::cout << "Done. Results in shadow_results.csv" << std::endl;
  if (!cfg.decision_log_path.empty()) {
    std::cout << "Decision trace: " << cfg.decision_log_path << std::endl;
  }
  hl::set_app_state(hl::AppState::ShuttingDown);
  hl::shutdown_logging();
  return 0;
}
