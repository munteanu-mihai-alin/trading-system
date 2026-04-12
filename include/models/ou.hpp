#pragma once

namespace hft {

struct OUState {
    double theta = 0.5;
    double mu = 100.0;
    double x = 100.0;

    void step(double dt) { x += theta * (mu - x) * dt; }
};

inline void update_ou(OUState& s, double observed) {
    const double err = observed - s.x;
    s.x += 0.1 * err;
}

}  // namespace hft
