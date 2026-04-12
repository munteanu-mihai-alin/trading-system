#include "bench/bench.hpp"
#include "common/TestFramework.hpp"
#include "config/LiveTradingConfig.hpp"
#include "core/ForecastNormalizer.h"
#include "core/portfolio.hpp"
#include "execution/fill_model.hpp"
#include "execution/latency_model.hpp"
#include "execution/score.hpp"
#include "models/hawkes.hpp"
#include "models/l2_book.hpp"
#include "models/micro.hpp"
#include "models/ou.hpp"
#include "models/stock.hpp"
#include "sim/queue_tracker.hpp"
#include "validation/validation.hpp"

using namespace hft;

HFT_TEST(test_hawkes_event_increases_lambda) {
    Hawkes h;
    const double before = h.lambda;
    h.update(0.1, 1);
    hft::test::require(h.lambda > before, "hawkes intensity should increase after event");
}

HFT_TEST(test_hawkes_decays_toward_mu_without_event) {
    Hawkes h;
    h.lambda = 25.0;
    const double next = h.one_step_decay(0.1);
    hft::test::require(next < h.lambda, "hawkes should decay toward baseline without event");
    hft::test::require(next > h.mu,
                       "hawkes decay should stay above baseline if starting above baseline");
}

HFT_TEST(test_ou_moves_toward_mean) {
    OUState s;
    s.x = 90.0;
    s.mu = 100.0;
    const double before = s.x;
    s.step(0.1);
    hft::test::require(s.x > before, "OU state below mean should move upward");
}

HFT_TEST(test_fill_probability_in_unit_interval) {
    FillModel m;
    const double p = m.compute(100.0, 200.0, 0.01);
    hft::test::require(p >= 0.0 && p <= 1.0, "fill probability must be in [0,1]");
}

HFT_TEST(test_fill_increases_with_traded_volume) {
    FillModel m;
    const double p1 = m.compute(10.0, 200.0, 0.01);
    const double p2 = m.compute(100.0, 200.0, 0.01);
    hft::test::require(p2 > p1, "more traded volume should increase fill probability");
}

HFT_TEST(test_execution_score_decreases_with_latency) {
    const double s1 = compute_execution_score(100, 99.9, 0.01, 50, 1000, 1);
    const double s2 = compute_execution_score(100, 99.9, 0.01, 50, 1000, 50);
    hft::test::require(s1 > s2, "higher latency should reduce execution score");
}

// ===== Branch coverage cases =====

HFT_TEST(test_microprice_between_bid_and_ask) {
    const double m = microprice(100.0, 100.2, 500.0, 300.0);
    hft::test::require(m >= 100.0 && m <= 100.2, "microprice must lie inside spread");
}

HFT_TEST(test_imbalance_positive_and_negative_paths) {
    hft::test::require(imbalance(200.0, 100.0) > 0.0,
                       "higher bid volume should mean positive imbalance");
    hft::test::require(imbalance(100.0, 200.0) < 0.0,
                       "higher ask volume should mean negative imbalance");
}

HFT_TEST(test_ou_update_moves_toward_observed) {
    OUState s;
    s.x = 100.0;
    update_ou(s, 110.0);
    hft::test::require(s.x > 100.0, "ou update should move toward higher observed value");
}

HFT_TEST(test_fill_probability_increases_when_distance_grows_cross_branch) {
    FillModel m;
    const double near_p = m.compute(0.0, 1000.0, 0.001);
    const double far_p = m.compute(0.0, 1000.0, 0.1);
    hft::test::require(far_p > near_p,
                       "crossing component should rise with distance parameter in current model");
}

HFT_TEST(test_validation_alarm_false_for_good_predictions) {
    ValidationMetrics v;
    for (int i = 0; i < 50; ++i) {
        const double p = (i % 2 == 0) ? 0.0 : 1.0;
        v.add(p, p);
    }
    hft::test::require(!v.degradation_alarm(0.35, 0.35, 0.60),
                       "good predictions should not trigger alarm");
}

HFT_TEST(test_latency_summary_empty_and_nonempty) {
    const auto empty = summarize_cycles({});
    hft::test::require_close(empty.avg, 0.0, 1e-12, "empty summary avg should be zero");

    const auto filled = summarize_cycles({10, 20, 30, 40, 50});
    hft::test::require(filled.max == 50.0, "max should be computed");
    hft::test::require(filled.avg > 0.0, "avg should be positive");
}

HFT_TEST(test_live_trading_config_mode_names) {
    AppConfig live_cfg;
    live_cfg.mode = BrokerMode::Live;
    hft::test::require(LiveTradingConfig::from_app(live_cfg).mode_name() == "live",
                       "live mode name should be live");

    AppConfig sim_cfg;
    sim_cfg.mode = BrokerMode::Sim;
    hft::test::require(LiveTradingConfig::from_app(sim_cfg).mode_name() == "sim",
                       "sim mode name should be sim");

    AppConfig paper_cfg;
    paper_cfg.mode = BrokerMode::Paper;
    hft::test::require(LiveTradingConfig::from_app(paper_cfg).mode_name() == "paper",
                       "paper mode name should be paper");
}

HFT_TEST(test_ranked_portfolio_sorts_descending_by_score) {
    RankedPortfolio<Stock> p;
    Stock a;
    a.score = 1.0;
    Stock b;
    b.score = 3.0;
    Stock c;
    c.score = 2.0;
    p.items = {a, b, c};
    p.rank();
    hft::test::require(p.items[0].score == 3.0, "highest score should sort first");
    hft::test::require(p.items[2].score == 1.0, "lowest score should sort last");
}

HFT_TEST(test_l2_book_best_prices_default_and_set) {
    L2Book book;
    hft::test::require_close(book.best_bid(), 0.0, 1e-12, "default best bid should be zero");
    hft::test::require_close(book.best_ask(), 0.0, 1e-12, "default best ask should be zero");
    book.bids[0] = {101.0, 10.0};
    book.asks[0] = {101.5, 12.0};
    hft::test::require_close(book.best_bid(), 101.0, 1e-12, "best bid should reflect first level");
    hft::test::require_close(book.best_ask(), 101.5, 1e-12, "best ask should reflect first level");
}

HFT_TEST(test_forecast_normalizer_zero_abs_mean_returns_zero) {
    ForecastNormalizer n;
    hft::test::require_close(n.normalize(5.0, 0.0), 0.0, 1e-12,
                             "zero historical abs mean should return zero");
}

HFT_TEST(test_fill_model_clamps_extreme_inputs) {
    FillModel m;
    const double low = m.compute(-100.0, 1.0, -1.0);
    const double high = m.compute(1e9, 1e-9, 1e9);
    hft::test::require(low >= 0.0 && low <= 1.0, "fill model should clamp low extreme");
    hft::test::require(high >= 0.0 && high <= 1.0, "fill model should clamp high extreme");
}

HFT_TEST(test_latency_model_mean_defaults_then_updates) {
    LatencyModel l;
    hft::test::require_close(l.mean_latency(), 1.0, 1e-12,
                             "empty latency model should default to 1ms");
    l.record(2.0);
    l.record(4.0);
    hft::test::require_close(l.mean_latency(), 3.0, 1e-12, "mean latency should average samples");
}

HFT_TEST(test_hawkes_decay_rises_toward_mu_when_below_baseline) {
    Hawkes h;
    h.lambda = 2.0;
    const double next = h.one_step_decay(0.1);
    hft::test::require(next > h.lambda,
                       "hawkes decay should move upward toward baseline when below mu");
}

HFT_TEST(test_ou_moves_downward_when_above_mean) {
    OUState s;
    s.x = 110.0;
    s.mu = 100.0;
    const double before = s.x;
    s.step(0.1);
    hft::test::require(s.x < before, "OU state above mean should move downward");
}

HFT_TEST(test_queue_tracker_clamps_negative_queue_to_zero) {
    MyOrderState s;
    s.reset(1, 100.0, 1.0);
    s.on_traded(10.0, 0.0);
    hft::test::require_close(s.queue_ahead, 0.0, 1e-12, "queue ahead should clamp at zero");
}

HFT_TEST(test_ranked_portfolio_preserves_equal_scores_count) {
    RankedPortfolio<Stock> p;
    Stock a;
    a.score = 2.0;
    Stock b;
    b.score = 2.0;
    Stock c;
    c.score = 2.0;
    p.items = {a, b, c};
    p.rank();
    hft::test::require(p.items.size() == 3, "equal-score ranking should preserve item count");
    hft::test::require_close(p.items[0].score, 2.0, 1e-12,
                             "equal-score ranking should keep valid values");
}

HFT_TEST(test_ranked_portfolio_handles_reverse_sorted_input) {
    RankedPortfolio<Stock> p;
    Stock a;
    a.score = 1.0;
    Stock b;
    b.score = 2.0;
    Stock c;
    c.score = 3.0;
    p.items = {a, b, c};
    p.rank();
    hft::test::require_close(p.items[0].score, 3.0, 1e-12, "reverse input should sort descending");
    hft::test::require_close(p.items[1].score, 2.0, 1e-12, "middle score should remain in middle");
    hft::test::require_close(p.items[2].score, 1.0, 1e-12, "lowest score should end last");
}

HFT_TEST(test_latency_model_single_sample_mean) {
    LatencyModel l;
    l.record(7.5);
    hft::test::require_close(l.mean_latency(), 7.5, 1e-12,
                             "single-sample mean should equal sample");
}

HFT_TEST(test_forecast_normalizer_negative_input_scales_and_caps) {
    ForecastNormalizer n;
    const double x = n.normalize(-100.0, 1.0);
    hft::test::require(x >= -20.0 && x <= 0.0,
                       "negative normalized forecast should be capped within bounds");
}

HFT_TEST(test_ranked_portfolio_orders_negative_and_positive_scores) {
    RankedPortfolio<Stock> p;
    Stock a;
    a.score = -1.0;
    Stock b;
    b.score = 0.5;
    Stock c;
    c.score = 0.0;
    p.items = {a, b, c};
    p.rank();
    hft::test::require_close(p.items[0].score, 0.5, 1e-12,
                             "highest positive score should be first");
    hft::test::require_close(p.items[2].score, -1.0, 1e-12, "negative score should be last");
}

HFT_TEST(test_latency_model_multiple_records_average) {
    LatencyModel m;
    m.record(1.0);
    m.record(3.0);
    m.record(5.0);
    hft::test::require_close(m.mean_latency(), 3.0, 1e-12,
                             "mean latency should average multiple samples");
}

HFT_TEST(test_trade_stats_all_losses_zero_win_rate) {
    TradeStats s;
    s.update(-1.0);
    s.update(-2.0);
    hft::test::require_close(s.win_rate(), 0.0, 1e-12,
                             "all-loss trade stats should have zero win rate");
}

HFT_TEST(test_l2_book_best_prices_after_partial_assignment) {
    L2Book b;
    b.bids[0] = {99.5, 3.0};
    hft::test::require_close(b.best_bid(), 99.5, 1e-12,
                             "best bid should reflect populated bid side");
    hft::test::require_close(b.best_ask(), 0.0, 1e-12, "empty ask side should stay zero");
}

HFT_TEST(test_stock_default_flags_and_fields) {
    Stock s;
    hft::test::require(!s.active, "stock should default to inactive");
    hft::test::require(s.cooldown == 0, "stock cooldown should default to zero");
    hft::test::require_close(s.score, 0.0, 1e-12, "stock score should default to zero");
}
