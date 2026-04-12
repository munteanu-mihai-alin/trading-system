#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace hft {

struct CalibrationBin {
    int count = 0;
    double predicted_sum = 0.0;
    double realized_sum = 0.0;
};

class ValidationMetrics {
    std::vector<double> predicted_;
    std::vector<double> realized_;
    std::vector<double> rolling_error_;
    std::vector<CalibrationBin> bins_{10};

   public:
    void add(double predicted, double realized) {
        predicted_.push_back(predicted);
        realized_.push_back(realized);

        const std::size_t idx =
            std::min<std::size_t>(9, static_cast<std::size_t>(predicted * 10.0));
        bins_[idx].count += 1;
        bins_[idx].predicted_sum += predicted;
        bins_[idx].realized_sum += realized;

        rolling_error_.push_back(std::fabs(predicted - realized));
        if (rolling_error_.size() > 100) {
            rolling_error_.erase(rolling_error_.begin());
        }
    }

    [[nodiscard]] double calibration_error() const {
        if (predicted_.empty()) return 0.0;
        double err = 0.0;
        for (std::size_t i = 0; i < predicted_.size(); ++i) {
            err += std::fabs(predicted_[i] - realized_[i]);
        }
        return err / static_cast<double>(predicted_.size());
    }

    [[nodiscard]] double rolling_error_mean() const {
        if (rolling_error_.empty()) return 0.0;
        double s = 0.0;
        for (double x : rolling_error_) s += x;
        return s / static_cast<double>(rolling_error_.size());
    }

    // Two-sample KS-like statistic over the observed values.
    [[nodiscard]] double ks_statistic() const {
        if (predicted_.empty() || realized_.empty()) return 0.0;

        std::vector<double> p = predicted_;
        std::vector<double> r = realized_;
        std::sort(p.begin(), p.end());
        std::sort(r.begin(), r.end());

        std::vector<double> grid = p;
        grid.insert(grid.end(), r.begin(), r.end());
        std::sort(grid.begin(), grid.end());

        double max_diff = 0.0;
        std::size_t ip = 0, ir = 0;
        for (double x : grid) {
            while (ip < p.size() && p[ip] <= x) ++ip;
            while (ir < r.size() && r[ir] <= x) ++ir;
            const double cdf_p = static_cast<double>(ip) / static_cast<double>(p.size());
            const double cdf_r = static_cast<double>(ir) / static_cast<double>(r.size());
            max_diff = std::max(max_diff, std::fabs(cdf_p - cdf_r));
        }
        return max_diff;
    }

    [[nodiscard]] std::vector<CalibrationBin> calibration_bins() const { return bins_; }

    [[nodiscard]] bool degradation_alarm(double calibration_threshold, double rolling_threshold,
                                         double ks_threshold) const {
        return calibration_error() > calibration_threshold ||
               rolling_error_mean() > rolling_threshold || ks_statistic() > ks_threshold;
    }

    [[nodiscard]] std::size_t size() const { return predicted_.size(); }
};

}  // namespace hft
