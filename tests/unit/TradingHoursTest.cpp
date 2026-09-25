// Tests for the NYSE regular-session gate.
//
// This exists because freshness is not the same question as
// tradeability. Verified live at 18:31 ET on a Friday: after-hours
// books were updating continuously and md read Ready, two and a half
// hours after the close. Entries would have priced against thin,
// wide, out-of-session liquidity that nothing in this strategy was
// backtested on.
//
// Normal operation is bounded by the RTH systemd timers; this guards
// the paths that bypass them -- the start timer's Persistent=true
// firing a missed run after a reboot, and manual `systemctl start`.
//
// Instants below are built in UTC and asserted through the tz
// database, so the DST cases genuinely exercise the EDT/EST offset
// rather than a hardcoded one.

#include <gtest/gtest.h>

#include <chrono>

#include "app/trading_hours.hpp"
#include "config/AppConfig.hpp"

namespace {

using namespace std::chrono;

// Build a UTC instant from Y/M/D and hh:mm.
system_clock::time_point utc(int y, unsigned m, unsigned d, int hh, int mm) {
  const year_month_day ymd{year{y}, month{m}, day{d}};
  return sys_days{ymd} + hours{hh} + minutes{mm};
}

// ---- EDT (UTC-4): summer. 13:30 UTC == 09:30 ET ----

TEST(TradingHoursTest, EdtOpenBoundary) {
  // Mon 2026-06-15. 13:29 UTC == 09:29 ET, one minute early.
  EXPECT_FALSE(hft::is_within_rth(utc(2026, 6, 15, 13, 29)));
  EXPECT_TRUE(hft::is_within_rth(utc(2026, 6, 15, 13, 30)));
}

TEST(TradingHoursTest, EdtCloseBoundaryIsExclusive) {
  // 19:59 UTC == 15:59 ET open; 20:00 UTC == 16:00 ET closed.
  EXPECT_TRUE(hft::is_within_rth(utc(2026, 6, 15, 19, 59)));
  EXPECT_FALSE(hft::is_within_rth(utc(2026, 6, 15, 20, 0)));
}

// ---- EST (UTC-5): winter. 14:30 UTC == 09:30 ET ----

TEST(TradingHoursTest, EstUsesTheShiftedOffset) {
  // Thu 2026-01-15. 13:30 UTC is 08:30 EST -- still closed, and the
  // case a hardcoded UTC-4 rule would get wrong.
  EXPECT_FALSE(hft::is_within_rth(utc(2026, 1, 15, 13, 30)));
  EXPECT_TRUE(hft::is_within_rth(utc(2026, 1, 15, 14, 30)));
  EXPECT_TRUE(hft::is_within_rth(utc(2026, 1, 15, 20, 59)));
  EXPECT_FALSE(hft::is_within_rth(utc(2026, 1, 15, 21, 0)));
}

// ---- weekends ----

TEST(TradingHoursTest, WeekendsAreClosed) {
  // Sat 2026-09-26 and Sun 2026-09-27, both mid-session by clock.
  EXPECT_FALSE(hft::is_within_rth(utc(2026, 9, 26, 15, 0)));
  EXPECT_FALSE(hft::is_within_rth(utc(2026, 9, 27, 15, 0)));
}

TEST(TradingHoursTest, FridayAfterHoursIsClosed) {
  // The live observation that motivated this: Fri 2026-09-25 22:31 UTC
  // == 18:31 ET. Books were fresh; the session was over.
  EXPECT_FALSE(hft::is_within_rth(utc(2026, 9, 25, 22, 31)));
}

TEST(TradingHoursTest, SundayCatchUpStartIsClosed) {
  // The Persistent=true reboot path: a missed run fires on a Sunday.
  EXPECT_FALSE(hft::is_within_rth(utc(2026, 9, 27, 13, 30)));
}

// ---- gate applicability ----

TEST(TradingHoursTest, BacktestIsNeverGated) {
  // A replay runs at whatever wall-clock time it happens to execute,
  // so "is the market open now" is meaningless and would refuse every
  // entry in the run.
  EXPECT_FALSE(hft::rth_gate_applies(true, hft::BrokerMode::DatabentoBacktest));
  EXPECT_FALSE(hft::rth_gate_applies(true, hft::BrokerMode::Sim));
}

TEST(TradingHoursTest, LiveModesHonourTheConfigFlag) {
  EXPECT_TRUE(hft::rth_gate_applies(true, hft::BrokerMode::IBKRPaper));
  EXPECT_TRUE(hft::rth_gate_applies(true, hft::BrokerMode::Live));
  EXPECT_FALSE(hft::rth_gate_applies(false, hft::BrokerMode::IBKRPaper));
  EXPECT_FALSE(hft::rth_gate_applies(false, hft::BrokerMode::Live));
}

}  // namespace
