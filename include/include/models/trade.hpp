#pragma once

namespace hft {

struct TradeStats {
    int trades = 0;
    double pnl = 0.0;
    int wins = 0;

    void update(double trade_pnl) {
        ++trades;
        pnl += trade_pnl;
        if (trade_pnl > 0.0) ++wins;
    }

    [[nodiscard]] double win_rate() const {
        if (trades == 0) return 0.0;
        return static_cast<double>(wins) / static_cast<double>(trades);
    }
};

}  // namespace hft
