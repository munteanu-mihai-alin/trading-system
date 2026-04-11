
#pragma once
#include <cmath>

struct FillModel {
    double compute(double traded_at_level, double queue_ahead, double dist){
        double p_queue = 1 - std::exp(- traded_at_level / (queue_ahead + 1e-9));
        double p_cross = 1 - std::exp(-5.0 * dist);
        return 1 - (1 - p_queue) * (1 - p_cross);
    }
};
