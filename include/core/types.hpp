#pragma once

namespace hft {

struct MarketEvent {
    int ticker_id = 0;
    double bid = 0.0;
    double ask = 0.0;
    double bid_vol = 0.0;
    double ask_vol = 0.0;
    double trade_price = 0.0;
    double trade_size = 0.0;
    double timestamp = 0.0;
};

}  // namespace hft
