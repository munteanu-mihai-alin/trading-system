
#pragma once
#include "models/hawkes.hpp"
#include "sim/queue_track.hpp"

struct Stock {
    double mid = 100;
    double queue = 500; // fallback
    double score = 0;
    double best_L = 0;

    Hawkes hawkes;
    MyOrderState my;
};
