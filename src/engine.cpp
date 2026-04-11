
#include <vector>
#include <algorithm>
#include <cmath>

#include "models/hawkes.hpp"
#include "fill/fill.hpp"
#include "sim/sim.hpp"

struct Metrics {
    int predicted_hits=0;
    int realized_hits=0;
};

struct Engine {

    FillModel fill;
    Simulator sim;
    Hawkes hawkes;
    Metrics metrics;

    void step(){
        double traded = sim.traded();
        double queue = (rand()%500)+1;
        double dist = 0.01;

        hawkes.update(0.001, rand()%2);

        double p = fill.compute(traded, queue, dist);

        bool predicted = p > 0.5;
        bool realized = sim.realized_fill(queue);

        if(predicted) metrics.predicted_hits++;
        if(realized) metrics.realized_hits++;
    }
};
