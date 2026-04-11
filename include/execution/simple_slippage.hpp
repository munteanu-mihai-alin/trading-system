
#pragma once
#include "interfaces/islippage.hpp"

struct SimpleSlippage : ISlippageModel {
    double estimate(double px, double qty) override {
        return px * 0.0001 * qty;
    }
};
