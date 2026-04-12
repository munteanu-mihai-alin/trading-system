#pragma once
#include <array>

namespace hft {

struct L2Level {
    double price = 0.0;
    double size = 0.0;
};

struct L2Book {
    static constexpr int DEPTH = 5;
    std::array<L2Level, DEPTH> bids{};
    std::array<L2Level, DEPTH> asks{};

    [[nodiscard]] double best_bid() const { return bids[0].price; }
    [[nodiscard]] double best_ask() const { return asks[0].price; }
};

}  // namespace hft
