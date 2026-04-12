#include "common/TestFramework.hpp"
#include "validation/validation.hpp"

using namespace hft;

HFT_TEST(test_calibration_error_zero_for_perfect_predictions) {
  ValidationMetrics v;
  v.add(0.0, 0.0);
  v.add(1.0, 1.0);
  hft::test::require_close(
      v.calibration_error(), 0.0, 1e-12,
      "perfect predictions should have zero calibration error");
}

HFT_TEST(test_ks_zero_for_identical_samples) {
  ValidationMetrics v;
  v.add(0.1, 0.1);
  v.add(0.8, 0.8);
  v.add(0.4, 0.4);
  hft::test::require_close(
      v.ks_statistic(), 0.0, 1e-12,
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

// ===== Branch coverage cases =====

HFT_TEST(test_validation_empty_paths) {
  ValidationMetrics v;
  hft::test::require_close(v.calibration_error(), 0.0, 1e-12,
                           "empty calibration should be zero");
  hft::test::require_close(v.rolling_error_mean(), 0.0, 1e-12,
                           "empty rolling error should be zero");
  hft::test::require_close(v.ks_statistic(), 0.0, 1e-12,
                           "empty ks should be zero");
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
  hft::test::require(v.rolling_error_mean() > 0.0,
                     "rolling error should remain positive");
}

HFT_TEST(test_degradation_alarm_false_for_clean_metrics) {
  ValidationMetrics v;
  for (int i = 0; i < 10; ++i) {
    v.add(0.0, 0.0);
  }
  hft::test::require(!v.degradation_alarm(0.35, 0.35, 0.60),
                     "clean metrics should not trigger alarm");
}

HFT_TEST(test_validation_gets_unknown_bin_clamped_to_last_bucket) {
  ValidationMetrics v;
  v.add(1.0, 1.0);
  const auto bins = v.calibration_bins();
  hft::test::require(bins[9].count == 1,
                     "probability of 1.0 should map to last bin");
}

HFT_TEST(test_degradation_alarm_triggers_on_calibration_only) {
  ValidationMetrics v;
  for (int i = 0; i < 20; ++i) {
    v.add(1.0, 0.0);
  }
  hft::test::require(v.degradation_alarm(0.10, 2.0, 2.0),
                     "calibration threshold alone should trigger alarm");
}

HFT_TEST(test_degradation_alarm_triggers_on_ks_only) {
  ValidationMetrics v;
  for (int i = 0; i < 10; ++i)
    v.add(0.0, 1.0);
  hft::test::require(v.degradation_alarm(2.0, 2.0, 0.10),
                     "ks threshold alone should trigger alarm");
}

HFT_TEST(test_validation_bin_zero_and_one_edges) {
  ValidationMetrics v;
  v.add(0.0, 0.0);
  v.add(1.0, 1.0);
  const auto bins = v.calibration_bins();
  hft::test::require(bins[0].count >= 1,
                     "zero probability should map to first bin");
  hft::test::require(bins[9].count >= 1,
                     "one probability should map to last bin");
}

HFT_TEST(test_validation_calibration_bins_empty_counts_zero) {
  ValidationMetrics v;
  const auto bins = v.calibration_bins();
  int total = 0;
  for (const auto& b : bins)
    total += b.count;
  hft::test::require(total == 0,
                     "empty validation bins should all have zero count");
}

HFT_TEST(test_validation_alarm_on_rolling_error_only) {
  ValidationMetrics v;
  for (int i = 0; i < 120; ++i) {
    v.add(1.0, 0.0);
  }
  hft::test::require(v.degradation_alarm(2.0, 0.5, 2.0),
                     "rolling error threshold alone should trigger alarm");
}
