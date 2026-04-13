#pragma once
#include <cmath>

namespace hft {

// Basic rank score used in the shadow ranking engine.
inline double compute_score(double mid, double limit, double lambda,
                            double queue) {
  const double dist = std::abs(mid - limit);
  const double p_touch = std::exp(-5.0 * dist);
  const double p_fill = 1.0 - std::exp(-lambda / (queue + 1e-9));
  return p_touch * p_fill;
}

}  // namespace hft
