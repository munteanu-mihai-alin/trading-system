#pragma once
#include <vector>

#include "bench/bench.hpp"
#include "core/portfolio.hpp"
#include "execution/fill_model.hpp"
#include "execution/score.hpp"
#include "io/logger.hpp"
#include "models/stock.hpp"
#include "sim/orderbook.hpp"
#include "sim/simulator.hpp"
#include "validation/validation.hpp"

namespace hft {

class RankingEngine {
    int top_k_;
    FillModel fill_model_;
    Simulator simulator_;
    OrderBook order_book_;
    Logger logger_;

    int my_id_counter_ = 100000;

public:
    RankedPortfolio<Stock> portfolio;
    ValidationMetrics validation;
    std::vector<std::uint64_t> cycle_samples;

    explicit RankingEngine(int top_k, const std::string& csv_path);

    void initialize(int n_stocks);
    void step(int t);
};

}  // namespace hft
