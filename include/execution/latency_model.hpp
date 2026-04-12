#pragma once
#include <vector>

namespace hft {

class LatencyModel {
    std::vector<double> samples_;

public:
    void record(double ms) { samples_.push_back(ms); }

    [[nodiscard]] double mean_latency() const {
        if (samples_.empty()) return 1.0;
        double s = 0.0;
        for (double v : samples_) s += v;
        return s / static_cast<double>(samples_.size());
    }
};

}  // namespace hft
