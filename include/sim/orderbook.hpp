#pragma once
#include <algorithm>
#include <deque>
#include <vector>

namespace hft {

struct OBOrder {
    int id = 0;
    double price = 0.0;
    double qty = 0.0;
    bool is_buy = true;
    bool is_mine = false;
};

struct Level {
    double price = 0.0;
    std::deque<OBOrder> queue;
};

class OrderBook {
    std::vector<Level> bids_;
    std::vector<Level> asks_;

    static void sort_sides(std::vector<Level>& bids, std::vector<Level>& asks) {
        std::sort(bids.begin(), bids.end(),
                  [](const Level& a, const Level& b) { return a.price > b.price; });
        std::sort(asks.begin(), asks.end(),
                  [](const Level& a, const Level& b) { return a.price < b.price; });
    }

   public:
    void add(const OBOrder& o) {
        auto& side = o.is_buy ? bids_ : asks_;
        for (auto& lvl : side) {
            if (lvl.price == o.price) {
                lvl.queue.push_back(o);
                return;
            }
        }
        Level lvl;
        lvl.price = o.price;
        lvl.queue.push_back(o);
        side.push_back(lvl);
    }

    bool cancel(int id) {
        for (auto* side : {&bids_, &asks_}) {
            for (auto& lvl : *side) {
                const auto it = std::find_if(lvl.queue.begin(), lvl.queue.end(),
                                             [&](const OBOrder& o) { return o.id == id; });
                if (it != lvl.queue.end()) {
                    lvl.queue.erase(it);
                    return true;
                }
            }
        }
        return false;
    }

    [[nodiscard]] double queue_ahead_at_level(double price, int my_id, bool is_buy) const {
        const auto& side = is_buy ? bids_ : asks_;
        for (const auto& lvl : side) {
            if (lvl.price != price) continue;
            double ahead = 0.0;
            for (const auto& o : lvl.queue) {
                if (o.id == my_id) break;
                ahead += o.qty;
            }
            return ahead;
        }
        return 0.0;
    }

    struct MatchResult {
        double traded_at_price = 0.0;
        double my_filled_qty = 0.0;
    };

    MatchResult match_at_price(double watch_price, int my_id) {
        sort_sides(bids_, asks_);
        MatchResult out{};

        while (!bids_.empty() && !asks_.empty() && bids_.front().price >= asks_.front().price) {
            auto& bl = bids_.front();
            auto& al = asks_.front();
            if (bl.queue.empty()) {
                bids_.erase(bids_.begin());
                continue;
            }
            if (al.queue.empty()) {
                asks_.erase(asks_.begin());
                continue;
            }

            auto& b = bl.queue.front();
            auto& a = al.queue.front();

            const double qty = std::min(b.qty, a.qty);
            const double px = al.price;

            if (px == watch_price) out.traded_at_price += qty;
            if (b.id == my_id && b.is_mine) out.my_filled_qty += qty;
            if (a.id == my_id && a.is_mine) out.my_filled_qty += qty;

            b.qty -= qty;
            a.qty -= qty;

            if (b.qty <= 1e-12) bl.queue.pop_front();
            if (a.qty <= 1e-12) al.queue.pop_front();

            if (bl.queue.empty()) bids_.erase(bids_.begin());
            if (al.queue.empty()) asks_.erase(asks_.begin());
        }

        return out;
    }

    [[nodiscard]] const std::vector<Level>& bids() const { return bids_; }
    [[nodiscard]] const std::vector<Level>& asks() const { return asks_; }
};

}  // namespace hft
