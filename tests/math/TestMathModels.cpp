#include "bench/bench.hpp"
#include "common/TestFramework.hpp"
#include "config/LiveTradingConfig.hpp"
#include "core/portfolio.hpp"
#include "execution/fill_model.hpp"
#include "execution/score.hpp"
#include "models/hawkes.hpp"
#include "models/l2_book.hpp"
#include "models/micro.hpp"
#include "models/ou.hpp"
#include "models/stock.hpp"
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
