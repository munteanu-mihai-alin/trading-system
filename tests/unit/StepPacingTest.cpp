// Tests for hft::effective_step_interval_ms - the helper main.cpp uses
// to decide how fast the engine's outer loop may spin.
//
// The live loop had no pacing at all. IBroker::on_step is a no-op that
// IBKRClient never overrides and nothing in the Chronos engine blocks,
// so live/paper spun at ~900k steps/sec: 8.5M configured steps burned
// through in ~9.3s CPU and the engine exited mid-session.
//
// The branch that matters most here is the backtest one. There step
// index t IS the market-data row index, so pacing a replay would turn
// seconds into days -- and a backtest launched with an inherited live
// config must not be able to do that. That check lives in the helper,
// not in config, and these tests pin it down.

#include <gtest/gtest.h>

#include "app/step_pacing.hpp"
#include "config/AppConfig.hpp"

namespace {

TEST(StepPacingTest, PacesLiveModes) {
  EXPECT_EQ(hft::effective_step_interval_ms(250, hft::BrokerMode::IBKRPaper),
            250);
  EXPECT_EQ(hft::effective_step_interval_ms(250, hft::BrokerMode::Live), 250);
  EXPECT_EQ(hft::effective_step_interval_ms(100, hft::BrokerMode::Paper), 100);
}

TEST(StepPacingTest, BacktestNeverPacesEvenWhenConfigured) {
  // The important one: a backtest inheriting a live config must still
  // replay flat out. 8.5M rows at 250ms would be ~24 days.
  EXPECT_EQ(
      hft::effective_step_interval_ms(250, hft::BrokerMode::DatabentoBacktest),
      0);
  EXPECT_EQ(
      hft::effective_step_interval_ms(5000, hft::BrokerMode::DatabentoBacktest),
      0);
}

TEST(StepPacingTest, NonPositiveIntervalDisablesPacing) {
  EXPECT_EQ(hft::effective_step_interval_ms(0, hft::BrokerMode::IBKRPaper), 0);
  EXPECT_EQ(hft::effective_step_interval_ms(-1, hft::BrokerMode::IBKRPaper), 0);
  EXPECT_EQ(hft::effective_step_interval_ms(0, hft::BrokerMode::Live), 0);
}

TEST(StepPacingTest, SimModeIsPacedLikeOtherNonBacktestModes) {
  // Sim is not a replay-by-row broker, so it follows the live rule.
  EXPECT_EQ(hft::effective_step_interval_ms(250, hft::BrokerMode::Sim), 250);
}

}  // namespace
