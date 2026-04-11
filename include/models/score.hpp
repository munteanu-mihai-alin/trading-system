
#pragma once
#include <cmath>

inline double compute_score(double mid, double L, double lambda, double q){
    double d = fabs(mid - L);
    double p_touch = exp(-5*d);
    double p_fill = 1 - exp(-lambda/(q+1e-9));
    return p_touch * p_fill;
}
