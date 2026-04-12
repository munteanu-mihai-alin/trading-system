#include "common/TestFramework.hpp"
#include "sim/orderbook.hpp"

using namespace hft;

HFT_TEST(test_fifo_match_fills_front_order_first) {
    OrderBook ob;
    ob.add(OBOrder{1, 100.0, 50.0, true, false});
    ob.add(OBOrder{2, 100.0, 50.0, true, true});    // ours behind
    ob.add(OBOrder{3, 100.0, 50.0, false, false});  // crossing ask

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
