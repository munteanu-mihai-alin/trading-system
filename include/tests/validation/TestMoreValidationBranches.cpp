#include "common/TestFramework.hpp"
#include "validation/validation.hpp"

using namespace hft;

HFT_TEST(test_validation_empty_paths) {
    ValidationMetrics v;
    hft::test::require_close(v.calibration_error(), 0.0, 1e-12, "empty calibration should be zero");
    hft::test::require_close(v.rolling_error_mean(), 0.0, 1e-12,
                             "empty rolling error should be zero");
    hft::test::require_close(v.ks_statistic(), 0.0, 1e-12, "empty ks should be zero");
}

HFT_TEST(test_validation_size_and_bins) {
    ValidationMetrics v;
    v.add(0.95, 1.0);
    v.add(0.05, 0.0);
    v.add(0.15, 1.0);
    hft::test::require(v.size() == 3, "size should track inserts");
    const auto bins = v.calibration_bins();
    hft::test::require(bins[9].count == 1, "0.95 should go to last bin");
}

HFT_TEST(test_validation_rolling_window_cap) {
    ValidationMetrics v;
    for (int i = 0; i < 150; ++i) {
        v.add(1.0, 0.0);
    }
    hft::test::require(v.rolling_error_mean() > 0.0, "rolling error should remain positive");
}
