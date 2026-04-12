#include "common/TestFramework.hpp"

#include "bench/bench.hpp"
#include "execution/fill_model.hpp"
#include "models/micro.hpp"
#include "models/ou.hpp"
#include "validation/validation.hpp"

using namespace hft;

HFT_TEST(test_microprice_between_bid_and_ask) {
    const double m = microprice(100.0, 100.2, 500.0, 300.0);
    hft::test::require(m >= 100.0 && m <= 100.2, "microprice must lie inside spread");
}

HFT_TEST(test_imbalance_positive_and_negative_paths) {
    hft::test::require(imbalance(200.0, 100.0) > 0.0, "higher bid volume should mean positive imbalance");
    hft::test::require(imbalance(100.0, 200.0) < 0.0, "higher ask volume should mean negative imbalance");
}

HFT_TEST(test_ou_update_moves_toward_observed) {
    OUState s;
    s.x = 100.0;
    update_ou(s, 110.0);
    hft::test::require(s.x > 100.0, "ou update should move toward higher observed value");
}

HFT_TEST(test_fill_probability_increases_when_distance_grows_cross_branch) {
    FillModel m;
    const double near_p = m.compute(0.0, 1000.0, 0.001);
    const double far_p = m.compute(0.0, 1000.0, 0.1);
    hft::test::require(far_p > near_p, "crossing component should rise with distance parameter in current model");
}

HFT_TEST(test_validation_alarm_false_for_good_predictions) {
    ValidationMetrics v;
    for (int i = 0; i < 50; ++i) {
        const double p = (i % 2 == 0) ? 0.0 : 1.0;
        v.add(p, p);
    }
    hft::test::require(!v.degradation_alarm(0.35, 0.35, 0.60), "good predictions should not trigger alarm");
}

HFT_TEST(test_latency_summary_empty_and_nonempty) {
    const auto empty = summarize_cycles({});
    hft::test::require_close(empty.avg, 0.0, 1e-12, "empty summary avg should be zero");

    const auto filled = summarize_cycles({10, 20, 30, 40, 50});
    hft::test::require(filled.max == 50.0, "max should be computed");
    hft::test::require(filled.avg > 0.0, "avg should be positive");
}
