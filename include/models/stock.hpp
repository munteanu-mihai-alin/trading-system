
#pragma once
#include <string>
#include "models/hawkes.hpp"
#include "models/trade.hpp"

struct Stock {
    std::string symbol;
    double mid=100;
    double queue=500;
    double best_L=0;
    double score=0;

    Hawkes hawkes;

    TradeStats real;
    TradeStats shadow;

    bool active=false;
    int cooldown=0;
};
