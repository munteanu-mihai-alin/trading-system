#include "common/TestFramework.hpp"

#include <cstdio>
#include <fstream>
#include <vector>

#include "config/AppConfig.hpp"
#include "core/ForecastNormalizer.h"
#include "execution/InstitutionalTransactionCostModel.h"
#include "execution/MarketImpactSlippageModel.h"
#include "models/trade.hpp"
#include "risk/EWMAVolatility.h"

using namespace hft;

HFT_TEST(test_forecast_normalizer_caps) {
    ForecastNormalizer n;
    const double x = n.normalize(100.0, 1.0);
    hft::test::require(x <= 20.0, "forecast normalizer should cap output");
}

HFT_TEST(test_transaction_cost_nonnegative) {
    InstitutionalTransactionCostModel m(0.005, 0.0005, 0.1);
    const double c = m.estimateCost(100, 200, 50, 1'000'000);
    hft::test::require(c >= 0.0, "cost must be non-negative");
}

HFT_TEST(test_slippage_moves_price_for_buy) {
    MarketImpactSlippageModel m(0.0005, 0.1);
    const double px = m.adjustExecutionPrice(100.0, true, 0.01);
    hft::test::require(px > 100.0, "buy slippage should increase execution price");
}

HFT_TEST(test_ewma_vol_positive) {
    EWMAVolatility v;
    const std::vector<double> returns{0.01, -0.005, 0.002};
    const double vol = v.annualizedVol(returns);
    hft::test::require(vol > 0.0, "volatility should be positive");
}

// ===== Branch coverage cases =====

HFT_TEST(test_trade_stats_zero_win_rate_when_no_trades) {
    TradeStats s;
    hft::test::require_close(s.win_rate(), 0.0, 1e-12, "empty trade stats should have zero win rate");
}

HFT_TEST(test_trade_stats_win_rate_updates) {
    TradeStats s;
    s.update(1.0);
    s.update(-1.0);
    s.update(2.0);
    hft::test::require_close(s.win_rate(), 2.0 / 3.0, 1e-12, "win rate should reflect positive pnl trades");
}

HFT_TEST(test_app_config_loads_live_mode_and_values) {
    const std::string path = "tmp_test_config_live.ini";
    {
        std::ofstream out(path);
        out << "[runtime]\n";
        out << "top_k=7\n";
        out << "steps=42\n";
        out << "[broker]\n";
        out << "mode=live\n";
        out << "host=10.0.0.2\n";
        out << "paper_port=7001\n";
        out << "live_port=7002\n";
        out << "client_id=99\n";
    }

    const auto cfg = AppConfig::load_from_file(path);
    hft::test::require(cfg.mode == BrokerMode::Live, "mode should parse as live");
    hft::test::require(cfg.host == "10.0.0.2", "host should parse");
    hft::test::require(cfg.paper_port == 7001, "paper port should parse");
    hft::test::require(cfg.live_port == 7002, "live port should parse");
    hft::test::require(cfg.client_id == 99, "client id should parse");
    hft::test::require(cfg.top_k == 7, "top_k should parse");
    hft::test::require(cfg.steps == 42, "steps should parse");
    hft::test::require(cfg.port() == 7002, "live port should be selected in live mode");
    std::remove(path.c_str());
}

HFT_TEST(test_app_config_loads_sim_mode) {
    const std::string path = "tmp_test_config_sim.ini";
    {
        std::ofstream out(path);
        out << "mode=sim\n";
    }
    const auto cfg = AppConfig::load_from_file(path);
    hft::test::require(cfg.mode == BrokerMode::Sim, "mode should parse as sim");
    hft::test::require(cfg.port() == cfg.paper_port, "sim should use paper port path");
    std::remove(path.c_str());
}


// ===== Additional coverage cases =====

HFT_TEST(test_app_config_unknown_mode_falls_back_to_paper) {
    const std::string path = "tmp_test_config_unknown.ini";
    {
        std::ofstream out(path);
        out << "# comment\n";
        out << "[broker]\n";
        out << "mode=weird\n";
        out << "badlinewithoutseparator\n";
        out << "\n";
    }
    const auto cfg = AppConfig::load_from_file(path);
    hft::test::require(cfg.mode == BrokerMode::Paper, "unknown mode should fall back to paper");
    hft::test::require(cfg.port() == cfg.paper_port, "paper path should use paper port");
    std::remove(path.c_str());
}
