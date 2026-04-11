
#pragma once
struct TradeStats {
    int trades=0;
    double pnl=0;

    void update(double x){
        trades++;
        pnl += x;
    }
};
