
#pragma once
struct MyOrderState {
    double price = 0.0;
    double queue_ahead = 0.0; // volume ahead of us at the level

    void on_add_ahead(double qty){
        queue_ahead += qty;
    }

    void on_traded(double traded_at_level){
        queue_ahead -= traded_at_level;
        if(queue_ahead < 0) queue_ahead = 0;
    }
};
