
#pragma once
#include <deque>
#include <vector>
#include <algorithm>

struct OBOrder {
    int id;
    double price;
    double qty;
    bool is_buy;
};

struct Trade {
    double price;
    double qty;
};

struct Level {
    double price;
    std::deque<OBOrder> q; // FIFO
};

class OrderBook {
public:
    std::vector<Level> bids, asks;

    void add(const OBOrder& o){
        auto& side = o.is_buy ? bids : asks;
        for(auto& l: side){
            if(l.price == o.price){
                l.q.push_back(o);
                return;
            }
        }
        Level l{ o.price };
        l.q.push_back(o);
        side.push_back(l);
    }

    // returns total traded at a given price level (for queue depletion tracking)
    double match_level(double price){
        double traded = 0.0;

        auto sort_books = [&](){
            std::sort(bids.begin(), bids.end(), [](auto&a,auto&b){return a.price>b.price;});
            std::sort(asks.begin(), asks.end(), [](auto&a,auto&b){return a.price<b.price;});
        };
        sort_books();

        while(!bids.empty() && !asks.empty() && bids[0].price >= asks[0].price){
            auto& bl = bids[0];
            auto& al = asks[0];

            if(bl.q.empty()){ bids.erase(bids.begin()); continue; }
            if(al.q.empty()){ asks.erase(asks.begin()); continue; }

            OBOrder& b = bl.q.front();
            OBOrder& a = al.q.front();

            double q = std::min(b.qty, a.qty);
            double px = al.price;

            if(px == price) traded += q;

            b.qty -= q;
            a.qty -= q;

            if(b.qty <= 0) bl.q.pop_front();
            if(a.qty <= 0) al.q.pop_front();

            if(bl.q.empty()) bids.erase(bids.begin());
            if(al.q.empty()) asks.erase(asks.begin());
        }
        return traded;
    }
};
