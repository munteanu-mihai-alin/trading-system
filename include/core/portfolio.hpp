#pragma once
#include <vector>
#include <algorithm>
#include <string>
#include "models/ou.hpp"
#include "models/hawkes.hpp"
#include "execution/latency_model.hpp"

struct StockState {
    std::string symbol;
    double mid = 100.0;
    double queue = 1000.0;

    OUState ou;
    Hawkes hawkes;
    LatencyModel latency;

    double score = 0.0;
    double best_limit = 0.0;
};

struct Portfolio {
    std::vector<StockState> stocks;

    void rank() {
        std::sort(stocks.begin(), stocks.end(),
            [](auto& a, auto& b){ return a.score > b.score; });
    }
};
