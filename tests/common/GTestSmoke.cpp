// Phase 0 smoke test for the new gtest+gmock wiring. The intent is purely
// to confirm that:
//   - GTest::gtest, GTest::gmock, and GTest::gtest_main resolve correctly,
//   - hft_lib links cleanly into a gtest binary alongside the legacy
//     hft_tests TestFramework binary,
//   - CTest discovers and runs hft_gtests.
// Real unit and module tests land in subsequent phases under tests/unit/ and
// tests/module/. This file should stay tiny.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "log/event_types.hpp"
#include "log/spsc_ring.hpp"

namespace {

// Sanity: gtest itself works.
TEST(GTestSmoke, BasicAssertion) {
  EXPECT_EQ(2 + 2, 4);
  EXPECT_THAT(std::string{"abc"}, ::testing::StrEq("abc"));
}

// Sanity: hft_lib headers compile and a trivial SPSC ring round-trip works.
// This is enough to confirm the gtest binary genuinely links the project lib.
TEST(GTestSmoke, SpscRingRoundTrip) {
  hft::log::SpscRing<int, 8> ring;
  EXPECT_TRUE(ring.empty());
  EXPECT_TRUE(ring.try_push(7));
  int out = 0;
  EXPECT_TRUE(ring.try_pop(out));
  EXPECT_EQ(out, 7);
  EXPECT_TRUE(ring.empty());
}

// Sanity: enum-to-string helpers from the logging module are usable.
TEST(GTestSmoke, EventEnumToString) {
  EXPECT_STREQ(hft::log::to_string(hft::log::AppState::Live), "Live");
  EXPECT_STREQ(hft::log::to_string(hft::log::ComponentState::Ready), "Ready");
}

// gmock smoke: a trivial mocked interface compiles and matches an expectation.
class IDummy {
 public:
  virtual ~IDummy() = default;
  virtual int compute(int x) = 0;
};

class MockDummy : public IDummy {
 public:
  MOCK_METHOD(int, compute, (int x), (override));
};

TEST(GTestSmoke, GmockBasicExpectation) {
  MockDummy m;
  EXPECT_CALL(m, compute(3)).WillOnce(::testing::Return(9));
  EXPECT_EQ(m.compute(3), 9);
}

}  // namespace
