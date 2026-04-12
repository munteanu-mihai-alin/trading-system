#include "common/TestFramework.hpp"
#include "sim/orderbook.hpp"
#include "sim/queue_tracker.hpp"

using namespace hft;

HFT_TEST(test_fifo_match_fills_front_order_first) {
    OrderBook ob;
    ob.add(OBOrder{1, 100.0, 50.0, true, false});
    ob.add(OBOrder{2, 100.0, 50.0, true, true});
    ob.add(OBOrder{3, 100.0, 50.0, false, false});

    const auto res = ob.match_at_price(100.0, 2);
    hft::test::require_close(res.my_filled_qty, 0.0, 1e-12,
                             "our order should not fill before front order");
}

HFT_TEST(test_queue_ahead_reports_front_volume) {
    OrderBook ob;
    ob.add(OBOrder{1, 100.0, 40.0, true, false});
    ob.add(OBOrder{2, 100.0, 60.0, true, true});
    const double q = ob.queue_ahead_at_level(100.0, 2, true);
    hft::test::require_close(q, 40.0, 1e-12, "queue ahead should equal prior quantity at level");
}

HFT_TEST(test_match_reports_traded_volume_at_watch_price) {
    OrderBook ob;
    ob.add(OBOrder{1, 100.0, 100.0, true, false});
    ob.add(OBOrder{2, 100.0, 100.0, false, false});
    const auto res = ob.match_at_price(100.0, 999);
    hft::test::require_close(res.traded_at_price, 100.0, 1e-12,
                             "traded volume at watch price should equal matched quantity");
}

// ===== Branch coverage cases =====

HFT_TEST(test_cancel_existing_and_missing_order_paths) {
    OrderBook ob;
    ob.add(OBOrder{1, 100.0, 10.0, true, false});
    hft::test::require(ob.cancel(1), "existing order should cancel");
    hft::test::require(!ob.cancel(99), "missing order should not cancel");
}

HFT_TEST(test_queue_ahead_zero_when_order_missing) {
    OrderBook ob;
    ob.add(OBOrder{1, 100.0, 10.0, true, false});
    const double q = ob.queue_ahead_at_level(101.0, 2, true);
    hft::test::require_close(q, 0.0, 1e-12, "queue ahead should be zero for missing level");
}

HFT_TEST(test_my_order_state_realized_fill_false_then_true) {
    MyOrderState s;
    s.reset(1, 100.0, 50.0);
    hft::test::require(!s.realized_fill(), "fresh order should not be realized fill");
    s.on_traded(50.0, 5.0);
    hft::test::require(s.realized_fill(), "positive fill qty should mark realized fill");
}

HFT_TEST(test_match_reports_my_fill_when_front_order_is_mine) {
    OrderBook ob;
    ob.add(OBOrder{10, 100.0, 25.0, true, true});
    ob.add(OBOrder{20, 100.0, 25.0, false, false});
    const auto res = ob.match_at_price(100.0, 10);
    hft::test::require_close(res.my_filled_qty, 25.0, 1e-12, "my front order should fill");
}
