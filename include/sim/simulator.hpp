#pragma once
#include <random>

#include "sim/orderbook.hpp"

namespace hft {

class Simulator {
    std::mt19937 rng_{42};
    std::uniform_real_distribution<> price_noise_{-0.02, 0.02};
    std::uniform_int_distribution<> qty_dist_{1, 200};

public:
    double mid = 100.0;
    int next_id = 1;

    void seed_book(OrderBook& ob, double center) {
        for (int i = 0; i < 5; ++i) {
            ob.add(OBOrder{next_id++, center - 0.01 * (i + 1), static_cast<double>(qty_dist_(rng_)), true, false});
            ob.add(OBOrder{next_id++, center + 0.01 * (i + 1), static_cast<double>(qty_dist_(rng_)), false, false});
        }
    }

    void external_flow(OrderBook& ob) {
        OBOrder m;
        m.id = next_id++;
        m.price = mid + price_noise_(rng_);
        m.qty = static_cast<double>(qty_dist_(rng_));
        m.is_buy = (rng_() % 2) == 0;
        m.is_mine = false;
        ob.add(m);
        mid += price_noise_(rng_);
    }
};

}  // namespace hft
