// Regression tests for hft::compute_effective_steps - the helper main.cpp
// uses to decide the engine's outer-loop length.
//
// The 2026-05-29 yen v2 disk-full crash was caused by main.cpp doing
//
//     int effective_steps = cfg.steps;
//     if (...) {
//       ...
//       if (broker_max > 0) {
//         effective_steps = broker_max;   // OVERWRITE - missing min()
//       }
//     }
//
// instead of `min(cfg.steps, broker_max)`. With Databento's MBP-1
// schema NVDA alone reported 25 million rows so the engine ran ~25 M
// steps instead of the configured 8.5 M cap, writing 108 GB of
// step_trace.csv and filling /dev/sdb. These tests pin down the four
// branches of the helper so the bug can't come back.

#include <gtest/gtest.h>

#include <climits>

#include "app/effective_steps.hpp"
#include "config/AppConfig.hpp"

namespace {

// The headline regression: configured ceiling MUST win over a larger
// broker_max. Pre-fix this returned broker_max (the bug). Post-fix it
// returns cfg.steps.
TEST(EffectiveSteps, CapsAtConfiguredCeilingWhenBrokerMaxIsLarger) {
  EXPECT_EQ(hft::compute_effective_steps(/*cfg_steps=*/8'500'000,
                                         /*auto_from_broker=*/true,
                                         hft::BrokerMode::DatabentoBacktest,
                                         /*broker_max=*/25'158'115),
            8'500'000);
}

// The same intent from the other side: when the broker reports a
// shorter data window, the loop honors that so we don't decision on
// stale prices past the end of the cache.
TEST(EffectiveSteps, ShortensToBrokerMaxWhenBrokerMaxIsSmaller) {
  EXPECT_EQ(hft::compute_effective_steps(/*cfg_steps=*/8'500'000,
                                         /*auto_from_broker=*/true,
                                         hft::BrokerMode::DatabentoBacktest,
                                         /*broker_max=*/3'120),
            3'120);
}

// When auto_from_broker is off, cfg.steps is the unconditional answer.
// Belts and braces: even with a positive broker_max it must be ignored.
TEST(EffectiveSteps, IgnoresBrokerMaxWhenAutoFromBrokerIsFalse) {
  EXPECT_EQ(hft::compute_effective_steps(/*cfg_steps=*/1'000,
                                         /*auto_from_broker=*/false,
                                         hft::BrokerMode::DatabentoBacktest,
                                         /*broker_max=*/999'999),
            1'000);
}

// Non-backtest modes (paper, live, ibkr_paper) report broker_max=0 in
// practice; even if some future broker returned a value, we never want
// to derive the loop length from real-time market data. Test both the
// "paper" and "ibkr_paper" enums to lock the gate.
TEST(EffectiveSteps, IgnoresBrokerMaxForLivePaperModes) {
  EXPECT_EQ(hft::compute_effective_steps(/*cfg_steps=*/777,
                                         /*auto_from_broker=*/true,
                                         hft::BrokerMode::Paper,
                                         /*broker_max=*/12'345),
            777);
  EXPECT_EQ(hft::compute_effective_steps(/*cfg_steps=*/777,
                                         /*auto_from_broker=*/true,
                                         hft::BrokerMode::IBKRPaper,
                                         /*broker_max=*/12'345),
            777);
  EXPECT_EQ(hft::compute_effective_steps(/*cfg_steps=*/777,
                                         /*auto_from_broker=*/true,
                                         hft::BrokerMode::Live,
                                         /*broker_max=*/12'345),
            777);
}

// broker_max == 0 means the broker had nothing useful to say (no
// L1/L2 loaded yet, fresh probe). Preserve cfg.steps in that case.
TEST(EffectiveSteps, PreservesConfiguredStepsWhenBrokerMaxIsZero) {
  EXPECT_EQ(hft::compute_effective_steps(/*cfg_steps=*/500'000,
                                         /*auto_from_broker=*/true,
                                         hft::BrokerMode::DatabentoBacktest,
                                         /*broker_max=*/0),
            500'000);
}

// Negative broker_max would only happen if max_replay_steps ever returned
// a signed underflow / sentinel. Treat it like zero (no useful data).
TEST(EffectiveSteps, PreservesConfiguredStepsWhenBrokerMaxIsNegative) {
  EXPECT_EQ(hft::compute_effective_steps(/*cfg_steps=*/500'000,
                                         /*auto_from_broker=*/true,
                                         hft::BrokerMode::DatabentoBacktest,
                                         /*broker_max=*/-1),
            500'000);
}

// Equality boundary: same value in, same value out. Catches an
// "off-by-one in the comparator" regression (using < instead of <=).
TEST(EffectiveSteps, EqualValuesReturnEither) {
  EXPECT_EQ(hft::compute_effective_steps(/*cfg_steps=*/12'345,
                                         /*auto_from_broker=*/true,
                                         hft::BrokerMode::DatabentoBacktest,
                                         /*broker_max=*/12'345),
            12'345);
}

// INT_MAX broker_max (the practical "no useful cap" case for a
// configured ceiling) - the ceiling wins.
TEST(EffectiveSteps, IntMaxBrokerMaxStillCappedByConfig) {
  EXPECT_EQ(hft::compute_effective_steps(/*cfg_steps=*/1'000'000,
                                         /*auto_from_broker=*/true,
                                         hft::BrokerMode::DatabentoBacktest,
                                         /*broker_max=*/INT_MAX),
            1'000'000);
}

}  // namespace
