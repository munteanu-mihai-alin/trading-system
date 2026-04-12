#include "common/TestFramework.hpp"
#include "validation/validation.hpp"

using namespace hft;

HFT_TEST(test_calibration_error_zero_for_perfect_predictions) {
    ValidationMetrics v;
    v.add(0.0, 0.0);
    v.add(1.0, 1.0);
    hft::test::require_close(v.calibration_error(), 0.0, 1e-12,
                             "perfect predictions should have zero calibration error");
}

HFT_TEST(test_ks_zero_for_identical_samples) {
    ValidationMetrics v;
    v.add(0.1, 0.1);
    v.add(0.8, 0.8);
    v.add(0.4, 0.4);
    hft::test::require_close(v.ks_statistic(), 0.0, 1e-12,
                             "identical empirical distributions should have zero KS");
}

HFT_TEST(test_alarm_triggers_for_bad_predictions) {
    ValidationMetrics v;
    for (int i = 0; i < 120; ++i) {
        v.add(0.9, 0.0);
    }
    hft::test::require(v.degradation_alarm(0.35, 0.35, 0.60),
                       "alarm should trigger for persistently bad predictions");
}

HFT_TEST(test_calibration_bins_collect_counts) {
    ValidationMetrics v;
    v.add(0.05, 0.0);
    v.add(0.15, 1.0);
    v.add(0.15, 0.0);
    const auto bins = v.calibration_bins();
    hft::test::require(bins[0].count == 1, "first bin should have one sample");
    hft::test::require(bins[1].count == 2, "second bin should have two samples");
}
