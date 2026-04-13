#pragma once
#include <cmath>
#include "execution/ISlippageModel.h"

namespace hft {

class MarketImpactSlippageModel : public ISlippageModel {
  double halfSpread_;
  double impactCoefficient_;

 public:
  MarketImpactSlippageModel(double spread, double impact)
      : halfSpread_(spread), impactCoefficient_(impact) {}

  [[nodiscard]] double adjustExecutionPrice(
      double midPrice, bool isBuy, double participationRate) const override {
    const double spreadAdj =
        isBuy ? midPrice * halfSpread_ : -midPrice * halfSpread_;
    double impactAdj =
        midPrice * impactCoefficient_ * std::sqrt(std::abs(participationRate));
    if (!isBuy)
      impactAdj *= -1.0;
    return midPrice + spreadAdj + impactAdj;
  }
};

}  // namespace hft
