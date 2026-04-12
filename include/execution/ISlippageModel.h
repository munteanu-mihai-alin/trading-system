#pragma once

namespace hft {

class ISlippageModel {
 public:
  virtual ~ISlippageModel() = default;
  virtual double adjustExecutionPrice(double midPrice, bool isBuy,
                                      double participationRate) const = 0;
};

}  // namespace hft
