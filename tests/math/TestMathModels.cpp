#include "common/TestFramework.hpp"
#include "execution/fill_model.hpp"
#include "execution/score.hpp"
#include "models/hawkes.hpp"
#include "models/ou.hpp"

using namespace hft;

HFT_TEST(test_hawkes_event_increases_lambda) {
    Hawkes h;
    const double before = h.lambda;
    h.update(0.1, 1);
    hft::test::require(h.lambda > before, "hawkes intensity should increase after event");
}

HFT_TEST(test_hawkes_decays_toward_mu_without_event) {
    Hawkes h;
    h.lambda = 25.0;
    const double next = h.one_step_decay(0.1);
    hft::test::require(next < h.lambda, "hawkes should decay toward baseline without event");
    hft::test::require(next > h.mu,
                       "hawkes decay should stay above baseline if starting above baseline");
}

HFT_TEST(test_ou_moves_toward_mean) {
    OUState s;
    s.x = 90.0;
    s.mu = 100.0;
    const double before = s.x;
    s.step(0.1);
    hft::test::require(s.x > before, "OU state below mean should move upward");
}

HFT_TEST(test_fill_probability_in_unit_interval) {
    FillModel m;
    const double p = m.compute(100.0, 200.0, 0.01);
    hft::test::require(p >= 0.0 && p <= 1.0, "fill probability must be in [0,1]");
}

HFT_TEST(test_fill_increases_with_traded_volume) {
    FillModel m;
    const double p1 = m.compute(10.0, 200.0, 0.01);
    const double p2 = m.compute(100.0, 200.0, 0.01);
    hft::test::require(p2 > p1, "more traded volume should increase fill probability");
}

HFT_TEST(test_execution_score_decreases_with_latency) {
    const double s1 = compute_execution_score(100, 99.9, 0.01, 50, 1000, 1);
    const double s2 = compute_execution_score(100, 99.9, 0.01, 50, 1000, 50);
    hft::test::require(s1 > s2, "higher latency should reduce execution score");
}
