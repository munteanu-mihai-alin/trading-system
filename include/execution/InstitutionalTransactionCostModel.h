#pragma once
#include <algorithm>
#include <cmath>
#include "execution/ITransactionCostModel.h"

namespace hft {

class InstitutionalTransactionCostModel : public ITransactionCostModel {
  double commissionPerShare_;
  double commissionMinPerOrder_;
  double halfSpread_;
  double impactCoefficient_;

 public:
  // 3-arg constructor preserved for back-compat with existing callers
  // (defaults commissionMinPerOrder_ to 0, i.e. pure per-share with no floor).
  InstitutionalTransactionCostModel(double commission, double spread,
                                    double impact)
      : commissionPerShare_(commission),
        commissionMinPerOrder_(0.0),
        halfSpread_(spread),
        impactCoefficient_(impact) {}

  // 4-arg constructor: commission per-share + per-order floor in dollars.
  // IBKR Pro Fixed: (0.005, 1.00). IBKR Pro Tiered: (0.0035, 0.35).
  InstitutionalTransactionCostModel(double commission, double commissionMin,
                                    double spread, double impact)
      : commissionPerShare_(commission),
        commissionMinPerOrder_(commissionMin),
        halfSpread_(spread),
        impactCoefficient_(impact) {}

  [[nodiscard]] double estimateCost(double currentPosition,
                                    double targetPosition, double price,
                                    double dailyVolume) const override {
    const double tradeSize = std::abs(targetPosition - currentPosition);
    if (tradeSize == 0.0)
      return 0.0;

    // Per-leg commission: max(per_share * qty, min_per_order). Floor is
    // skipped when min == 0 to preserve the 3-arg-constructor behaviour
    // (legacy callers that don't model the per-order minimum get exactly
    // the same numbers as before).
    double commissionCost = tradeSize * commissionPerShare_;
    if (commissionMinPerOrder_ > 0.0) {
      commissionCost = std::max(commissionCost, commissionMinPerOrder_);
    }
    const double spreadCost = tradeSize * price * halfSpread_;
    const double participationRate = tradeSize / dailyVolume;
    const double impactCost = impactCoefficient_ * price * tradeSize *
                              std::sqrt(std::abs(participationRate));
    return commissionCost + spreadCost + impactCost;
  }
};

}  // namespace hft
