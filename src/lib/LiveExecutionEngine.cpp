#include "engine/LiveExecutionEngine.hpp"
#include "broker/IBKRClient.hpp"

#include <utility>

namespace hft {

LiveExecutionEngine::LiveExecutionEngine(LiveTradingConfig cfg,
                                         std::unique_ptr<IBroker> broker)
    : cfg_(std::move(cfg)),
      broker_(std::move(broker)),
      ranking(cfg_.app.top_k, "shadow_results.csv") {}

bool LiveExecutionEngine::start() {
  return broker_->connect(cfg_.app.host, cfg_.app.port(), cfg_.app.client_id);
}

void LiveExecutionEngine::stop() {
  broker_->disconnect();
}

void LiveExecutionEngine::initialize_universe(int n_stocks) {
  ranking.initialize(n_stocks);
}

void LiveExecutionEngine::step(int t) {
  reconcile_broker_state();
  ranking.step(t);

  for (const auto& s : ranking.portfolio.items) {
    if (!s.active)
      continue;

    OrderRequest req;
    req.id = next_order_id_++;
    req.symbol = s.symbol;
    req.is_buy = true;
    req.qty = 10.0;
    req.limit = s.best_limit;
    broker_->place_limit_order(req);
  }
}

}  // namespace hft

namespace hft {

void LiveExecutionEngine::subscribe_live_books(
    const std::vector<std::string>& symbols) {
  int ticker = 1;
  for (const auto& sym : symbols) {
    broker_->subscribe_market_depth(MarketDepthRequest{ticker++, sym, 5});
  }
}

void LiveExecutionEngine::reconcile_broker_state() {
  auto* ibkr = dynamic_cast<IBKRClient*>(broker_.get());
  if (ibkr == nullptr)
    return;

  for (std::size_t i = 0; i < ranking.portfolio.items.size(); ++i) {
    auto& s = ranking.portfolio.items[i];
    const auto book = ibkr->snapshot_book(static_cast<int>(i + 1));
    if (book.best_bid() > 0.0 && book.best_ask() > 0.0) {
      s.mid = 0.5 * (book.best_bid() + book.best_ask());
      if (book.bids[0].size > 0.0) {
        s.queue = book.bids[0].size;
      }
    }
  }
  ranking.portfolio.rank();
}

}  // namespace hft
