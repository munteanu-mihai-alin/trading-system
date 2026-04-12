#pragma once
#include <cmath>
#include "execution/ITransactionCostModel.h"

namespace hft {

class InstitutionalTransactionCostModel : public ITransactionCostModel {
    double commissionPerShare_;
    double halfSpread_;
    double impactCoefficient_;

public:
    InstitutionalTransactionCostModel(double commission,
                                      double spread,
                                      double impact)
        : commissionPerShare_(commission),
          halfSpread_(spread),
          impactCoefficient_(impact) {}

    [[nodiscard]] double estimateCost(double currentPosition,
                                      double targetPosition,
                                      double price,
                                      double dailyVolume) const override {
        const double tradeSize = std::abs(targetPosition - currentPosition);
        if (tradeSize == 0.0) return 0.0;

        const double commissionCost = tradeSize * commissionPerShare_;
        const double spreadCost = tradeSize * price * halfSpread_;
        const double participationRate = tradeSize / dailyVolume;
        const double impactCost = impactCoefficient_ * price *
                                  tradeSize * std::sqrt(std::abs(participationRate));
        return commissionCost + spreadCost + impactCost;
    }
};

}  // namespace hft
