
#pragma once

struct TradeStats {
    int trades = 0;
    double pnl = 0;
    int wins = 0;

    inline void update(double trade_pnl){
        trades++;
        pnl += trade_pnl;
        if(trade_pnl > 0) wins++;
    }
};
