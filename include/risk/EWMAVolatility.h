#pragma once
#include <cmath>

#include "risk/IVolatilityModel.h"

namespace hft {

class EWMAVolatility : public IVolatilityModel {
  double lambda_;
  double tradingDays_;

 public:
  EWMAVolatility(double lambda = 0.94, double td = 252.0)
      : lambda_(lambda), tradingDays_(td) {}

  [[nodiscard]] double annualizedVol(
      const std::vector<double>& returns) const override {
    double var = 0.0;
    for (double r : returns)
      var = lambda_ * var + (1.0 - lambda_) * r * r;
    return std::sqrt(var) * std::sqrt(tradingDays_);
  }
};

}  // namespace hft
