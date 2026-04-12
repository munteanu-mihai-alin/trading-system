#pragma once

namespace hft {

class ITransactionCostModel {
public:
    virtual ~ITransactionCostModel() = default;
    virtual double estimateCost(double currentPosition,
                                double targetPosition,
                                double price,
                                double dailyVolume) const = 0;
};

}  // namespace hft
