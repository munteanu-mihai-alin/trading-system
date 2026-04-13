#pragma once
#include <cmath>

namespace hft {

// Execution-adjusted score with latency and a simple reward/loss model.
inline double compute_execution_score(double mid, double limit, double mu,
                                      double lambda, double queue,
                                      double latency_ms) {
  const double dist = std::abs(mid - limit);
  const double p_touch = std::exp(-5.0 * dist);
  const double effective_queue = queue + lambda * (latency_ms / 1000.0);
  const double fill = 1.0 - std::exp(-lambda / (effective_queue + 1e-9));
  const double p_fill = p_touch * fill;

  const double reward = 0.8;
  const double loss = 0.5;
  const double p_up = 0.5 + mu * 0.1;
  const double ev = p_up * reward - (1.0 - p_up) * loss;

  return p_fill * ev;
}

}  // namespace hft
