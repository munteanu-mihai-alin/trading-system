#pragma once

namespace hft {

struct MyOrderState {
    int id = 0;
    double price = 0.0;
    double queue_ahead = 0.0;
    double filled_qty = 0.0;

    void reset(int new_id, double new_price, double ahead) {
        id = new_id;
        price = new_price;
        queue_ahead = ahead;
        filled_qty = 0.0;
    }

    void on_traded(double traded_at_level, double my_fill_qty) {
        queue_ahead -= traded_at_level;
        if (queue_ahead < 0.0) queue_ahead = 0.0;
        filled_qty += my_fill_qty;
    }

    [[nodiscard]] bool realized_fill() const {
        return filled_qty > 0.0;
    }
};

}  // namespace hft
