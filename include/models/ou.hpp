
#pragma once
#include <cmath>

struct OU {
    double theta=0.5;
    double mu=100;
    double x=100;

    void step(double dt){
        x += theta*(mu - x)*dt;
    }
};
