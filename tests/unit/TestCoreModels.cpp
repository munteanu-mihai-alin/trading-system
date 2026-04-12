#include <vector>

#include "common/TestFramework.hpp"
#include "core/ForecastNormalizer.h"
#include "execution/InstitutionalTransactionCostModel.h"
#include "execution/MarketImpactSlippageModel.h"
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
