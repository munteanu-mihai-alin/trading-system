
#pragma once
#include <random>

struct Simulator {
    std::mt19937 rng{42};

    double traded(){ return (rng()%500)+1; }

    bool realized_fill(double queue){
        return queue < 10; // simplistic: if near front, filled
    }
};
