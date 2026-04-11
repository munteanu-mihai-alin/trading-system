
#pragma once
struct ISlippageModel {
    virtual double estimate(double px, double qty)=0;
    virtual ~ISlippageModel() = default;
};
