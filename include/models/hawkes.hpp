#pragma once
#include <cmath>

namespace hft {

struct Hawkes {
    double mu = 10.0;
    double alpha = 5.0;
    double beta = 20.0;
    double lambda = 10.0;

    void update(double dt, int event) {
        const double decay = std::exp(-beta * dt);
        lambda = mu + (lambda - mu) * decay + alpha * static_cast<double>(event);
    }

    [[nodiscard]] double one_step_decay(double dt) const {
        return mu + (lambda - mu) * std::exp(-beta * dt);
    }
};

}  // namespace hft
