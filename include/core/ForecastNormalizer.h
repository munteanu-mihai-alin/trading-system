#pragma once
#include <algorithm>

namespace hft {

class ForecastNormalizer {
  double targetAbsMean_;
  double cap_;

 public:
  ForecastNormalizer(double target = 10.0, double cap = 20.0)
      : targetAbsMean_(target), cap_(cap) {}

  [[nodiscard]] double normalize(double raw, double historicalAbsMean) const {
    if (historicalAbsMean == 0.0)
      return 0.0;
    const double scaled = raw * (targetAbsMean_ / historicalAbsMean);
    return std::clamp(scaled, -cap_, cap_);
  }
};

}  // namespace hft
