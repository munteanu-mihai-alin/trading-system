#include "engine/RankingEngine.hpp"

#include <algorithm>
#include <cmath>
#include <random>

#include "bench/rdtsc.hpp"
#include "models/score.hpp"

namespace hft {

RankingEngine::RankingEngine(int top_k, const std::string& csv_path)
    : top_k_(top_k), logger_(csv_path) {
  simulator_.seed_book(order_book_, simulator_.mid);
}

void RankingEngine::initialize(int n_stocks) {
  portfolio.items.clear();
  for (int i = 0; i < n_stocks; ++i) {
    Stock s;
    s.symbol = "STK" + std::to_string(i);
    s.mid = 100.0 + 0.05 * static_cast<double>(i);
    portfolio.items.push_back(s);
  }
}

void RankingEngine::step(int t) {
  const auto start = rdtsc_start();

  // Update simulator with exogenous flow.
  for (int i = 0; i < 4; ++i)
    simulator_.external_flow(order_book_);

  for (auto& s : portfolio.items) {
    const int event = ((t + static_cast<int>(s.symbol.back())) % 2);
    s.hawkes.update(0.001, event);

    if (s.cooldown > 0)
      --s.cooldown;

    double best_score = -1e18;
    double best_limit = s.mid;

    for (int i = 0; i < 8; ++i) {
      const double L = s.mid - 0.10 + 0.025 * static_cast<double>(i);
      const double dist = std::abs(s.mid - L);

      // Place our synthetic order and track exact queue ahead.
      const int my_id = my_id_counter_++;
      order_book_.add(OBOrder{my_id, L, 50.0, true, true});
      const double queue_ahead =
          order_book_.queue_ahead_at_level(L, my_id, true);

      auto match = order_book_.match_at_price(L, my_id);
      const double p_pred =
          fill_model_.compute(match.traded_at_price, queue_ahead, dist);
      const double realized = (match.my_filled_qty > 0.0) ? 1.0 : 0.0;
      validation.add(p_pred, realized);

      const double sc = p_pred * s.hawkes.lambda;
      if (sc > best_score) {
        best_score = sc;
        best_limit = L;
        s.my_order.reset(my_id, L, queue_ahead);
        s.my_order.on_traded(match.traded_at_price, match.my_filled_qty);
      }
    }

    s.best_limit = best_limit;
    s.score = best_score;
    if (s.cooldown > 0)
      s.score *= 0.1;
  }

  portfolio.rank();

  for (std::size_t i = 0; i < portfolio.items.size(); ++i) {
    portfolio.items[i].active = static_cast<int>(i) < top_k_;
  }

  // Shadow-vs-real bookkeeping with a light simulated PnL.
  for (auto& s : portfolio.items) {
    const bool signal = ((t + static_cast<int>(s.symbol.size())) % 5 == 0);
    if (!signal)
      continue;

    const double pnl = std::sin(0.01 * static_cast<double>(t) + s.score) * 0.01;

    if (s.active) {
      s.real.update(pnl);
      logger_.log(t, s.symbol, "real", pnl, s.real.pnl, s.real.trades);
      if (pnl > 0.0)
        s.cooldown = 50;
    } else {
      s.shadow.update(pnl);
      logger_.log(t, s.symbol, "shadow", pnl, s.shadow.pnl, s.shadow.trades);
    }
  }

  const auto end = rdtsc_end();
  cycle_samples.push_back(end - start);
}

}  // namespace hft
