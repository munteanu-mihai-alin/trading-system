
#pragma once
#include <cmath>

struct Hawkes {
    double mu = 10;
    double alpha = 5;
    double beta = 20;
    double lambda = 10;

    inline void update(double dt, int event) {
        double decay = std::exp(-beta * dt);
        lambda = mu + (lambda - mu) * decay + alpha * event;
    }
};
