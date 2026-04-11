
#pragma once
#include <random>
#include "sim/orderbook.hpp"

struct Simulator {
    std::mt19937 rng{42};
    std::uniform_real_distribution<> noise{-0.02, 0.02};

    double mid = 100.0;

    // generate external flow and match; returns traded volume at target price
    double step(OrderBook& ob, double target_price){
        // random external order
        OBOrder m;
        m.id = rng();
        m.price = mid + noise(rng);
        m.qty = (rng()%200) + 1;
        m.is_buy = (rng()%2)==0;
        ob.add(m);

        // small drift
        mid += noise(rng);

        // match and get traded at our level
        double traded = ob.match_level(target_price);
        return traded;
    }
};
