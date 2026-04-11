#pragma once

struct OUState {
    double mean=0;
    double a=0.99;
};

inline void update_ou(OUState& s, double x) {
    double pred = s.a * s.mean;
    double err = x - pred;
    s.mean += 0.1 * err;
}
