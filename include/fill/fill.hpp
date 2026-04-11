
#pragma once
#include <cmath>

struct FillModel {
    double compute(double traded, double queue, double dist){
        double p_queue = 1 - std::exp(-traded/(queue+1e-9));
        double p_cross = 1 - std::exp(-5*dist);
        return 1 - (1-p_queue)*(1-p_cross);
    }
};
