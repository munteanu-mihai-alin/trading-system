#pragma once
#include <cmath>

inline double microprice(double bid, double ask, double bid_vol, double ask_vol) {
    double d = bid_vol + ask_vol + 1e-12;
    return (ask * bid_vol + bid * ask_vol) / d;
}

inline double imbalance(double bid_vol, double ask_vol) {
    return (bid_vol - ask_vol) / (bid_vol + ask_vol + 1e-12);
}
