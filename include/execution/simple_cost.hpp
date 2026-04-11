
#pragma once
#include "interfaces/icost.hpp"

struct SimpleCost : ICostModel {
    double cost(double qty) override {
        return 0.01 * qty;
    }
};
