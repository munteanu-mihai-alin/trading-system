
#pragma once
#include <cmath>
struct Hawkes {
    double mu=10, alpha=5, beta=20, lambda=10;
    void update(double dt, int event){
        double decay = std::exp(-beta*dt);
        lambda = mu + (lambda-mu)*decay + alpha*event;
    }
};
