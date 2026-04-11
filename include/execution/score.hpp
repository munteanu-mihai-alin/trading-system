#pragma once
#include <cmath>

inline double compute_score(double mid,double L,double mu,double lambda,double queue,double latency_ms) {
    double dist = fabs(mid - L);
    double p_touch = exp(-5.0 * dist);

    double effective_queue = queue + lambda * (latency_ms / 1000.0);
    double fill = 1 - exp(-lambda / (effective_queue + 1e-9));

    double p_fill = p_touch * fill;

    double reward = 0.8;
    double loss = 0.5;

    double p_up = 0.5 + mu * 0.1;
    double ev = p_up*reward - (1-p_up)*loss;

    return p_fill * ev;
}
