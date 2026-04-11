#include "engine/LiveExecutionEngine.hpp"

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
    ranking.step(t);

    for (const auto& s : ranking.portfolio.items) {
        if (!s.active) continue;

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
