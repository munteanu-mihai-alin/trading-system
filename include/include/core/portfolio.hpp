#pragma once
#include <algorithm>
#include <vector>

namespace hft {

template <class T>
struct RankedPortfolio {
    std::vector<T> items;

    void rank() {
        std::sort(items.begin(), items.end(),
                  [](const T& a, const T& b) { return a.score > b.score; });
    }
};

}  // namespace hft
