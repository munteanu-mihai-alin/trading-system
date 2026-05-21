#pragma once
#include <algorithm>
#include <cstddef>
#include <vector>

namespace hft {

// RankedPortfolio keeps `items` in *stable subscribe order* (the order in
// which symbols were originally added) and exposes a separate
// `ranked_indices` view that holds indices into `items` sorted by score
// descending. Callers that need stable-order iteration (e.g. ticker_id
// lookup that uses `items[i]` with `ticker_id = i + 1`) iterate `items`
// directly; callers that need ranked-order iteration (e.g. the
// "consider top-K candidates by score" path) iterate `ranked_indices`
// and index back into `items`.
//
// Historical note: this was previously a `std::sort` on `items` in
// place, which broke the ticker_id <-> portfolio-index alignment between
// the broker (which stored series at ticker_id == subscribe-order + 1)
// and the engine's reconcile loop (which assumed `items[i]` corresponded
// to subscribe-order i). See AGENT_HANDOFF_LOG entry
// "10-day Databento backtest completed end-to-end + ticker_id cross-wire
// found" for the smoking-gun trace.
template <class T>
struct RankedPortfolio {
  std::vector<T> items;
  std::vector<std::size_t> ranked_indices;

  void rank() {
    ranked_indices.resize(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
      ranked_indices[i] = i;
    }
    std::sort(ranked_indices.begin(), ranked_indices.end(),
              [this](std::size_t a, std::size_t b) {
                return items[a].score > items[b].score;
              });
  }
};

}  // namespace hft
