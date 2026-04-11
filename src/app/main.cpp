#include <iostream>

#include "bench/bench.hpp"
#include "config/AppConfig.hpp"
#include "engine/RankingEngine.hpp"

int main() {
    const auto cfg = hft::AppConfig::load_from_file("config.ini");

    hft::RankingEngine engine(cfg.top_k, "shadow_results.csv");
    engine.initialize(30);

    for (int t = 0; t < cfg.steps; ++t) {
        engine.step(t);
    }

    const auto summary = hft::summarize_cycles(engine.cycle_samples);
    std::cout << summary << "\n";
    std::cout << "Validation: calibration=" << engine.validation.calibration_error()
              << " rolling=" << engine.validation.rolling_error_mean()
              << " ks=" << engine.validation.ks_statistic()
              << " alarm=" << (engine.validation.degradation_alarm(0.35, 0.35, 0.60) ? "ON" : "OFF")
              << "\n";
    std::cout << "Done. Results in shadow_results.csv\n";
    return 0;
}
