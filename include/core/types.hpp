#pragma once

struct MarketEvent {
    int ticker_id;
    double bid, ask;
    double bid_vol, ask_vol;
    double trade_price;
    double trade_size;
    double timestamp;
};
