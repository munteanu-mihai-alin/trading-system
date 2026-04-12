#pragma once
#include <cmath>

namespace hft {

// Combined passive-queue and crossing-based fill approximation.
// p_queue captures depletion at our price level.
// p_cross captures the chance price moves through our order.
class FillModel {
 public:
  [[nodiscard]] double compute(double tradedAtLevel, double queueAhead,
                               double distanceFromMid) const {
    const double p_queue = 1.0 - std::exp(-tradedAtLevel / (queueAhead + 1e-9));
    const double p_cross = 1.0 - std::exp(-5.0 * distanceFromMid);
    const double p_fill = 1.0 - (1.0 - p_queue) * (1.0 - p_cross);
    if (p_fill < 0.0)
      return 0.0;
    if (p_fill > 1.0)
      return 1.0;
    return p_fill;
  }
};

}  // namespace hft
