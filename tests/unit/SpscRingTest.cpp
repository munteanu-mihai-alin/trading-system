// Unit tests for hft::log::SpscRing<T, Capacity>.
//
// The ring is the producer-side primitive used by every component that emits
// state-centric events. It must reject pushes when full, return false on pop
// from empty, wrap correctly, and report empty() consistently with the
// head/tail atomics.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

#include "log/spsc_ring.hpp"

namespace {

using hft::log::SpscRing;

TEST(SpscRing, NewRingIsEmpty) {
  SpscRing<int, 8> ring;
  EXPECT_TRUE(ring.empty());
  int out = 42;
  EXPECT_FALSE(ring.try_pop(out));
  // Output unchanged on failed pop.
  EXPECT_EQ(out, 42);
}

TEST(SpscRing, CapacityIsCapacityMinusOne) {
  // The ring reserves one slot to disambiguate full from empty.
  EXPECT_EQ((SpscRing<int, 8>::capacity()), 7u);
  EXPECT_EQ((SpscRing<int, 16>::capacity()), 15u);
  EXPECT_EQ((SpscRing<int, 2>::capacity()), 1u);
}

TEST(SpscRing, PushPopRoundTripPreservesValue) {
  SpscRing<int, 8> ring;
  ASSERT_TRUE(ring.try_push(7));
  EXPECT_FALSE(ring.empty());
  int out = 0;
  ASSERT_TRUE(ring.try_pop(out));
  EXPECT_EQ(out, 7);
  EXPECT_TRUE(ring.empty());
}

TEST(SpscRing, FifoOrderingPreserved) {
  SpscRing<int, 8> ring;
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(ring.try_push(i));
  }
  for (int i = 0; i < 5; ++i) {
    int out = -1;
    ASSERT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out, i);
  }
  EXPECT_TRUE(ring.empty());
}

TEST(SpscRing, PushReturnsFalseWhenFull) {
  SpscRing<int, 4> ring;  // usable capacity 3
  EXPECT_TRUE(ring.try_push(1));
  EXPECT_TRUE(ring.try_push(2));
  EXPECT_TRUE(ring.try_push(3));
  // Now full: head+1 == tail.
  EXPECT_FALSE(ring.try_push(4));
  // Confirm pre-existing values still readable in order.
  int out = 0;
  ASSERT_TRUE(ring.try_pop(out));
  EXPECT_EQ(out, 1);
  // After one pop a slot is free; push succeeds again.
  EXPECT_TRUE(ring.try_push(99));
}

TEST(SpscRing, WrapAroundFifoIntact) {
  SpscRing<int, 4> ring;  // usable capacity 3
  // Fill, drain, fill, drain - exercises the modulo wrap.
  for (int round = 0; round < 5; ++round) {
    EXPECT_TRUE(ring.try_push(round * 10 + 1));
    EXPECT_TRUE(ring.try_push(round * 10 + 2));
    EXPECT_TRUE(ring.try_push(round * 10 + 3));
    EXPECT_FALSE(ring.try_push(round * 10 + 4));  // full
    int out = 0;
    ASSERT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out, round * 10 + 1);
    ASSERT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out, round * 10 + 2);
    ASSERT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out, round * 10 + 3);
    EXPECT_TRUE(ring.empty());
  }
}

TEST(SpscRing, WorksWithPodStruct) {
  struct Pod {
    std::uint64_t a;
    std::uint32_t b;
  };
  SpscRing<Pod, 8> ring;
  ASSERT_TRUE(ring.try_push(Pod{123, 456}));
  Pod out{};
  ASSERT_TRUE(ring.try_pop(out));
  EXPECT_EQ(out.a, 123u);
  EXPECT_EQ(out.b, 456u);
}

}  // namespace
