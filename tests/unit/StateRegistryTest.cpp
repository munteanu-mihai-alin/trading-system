// Unit tests for hft::log::StateRegistry.
//
// The registry is the source of truth for "current state of each component".
// Producers update it via atomic exchange so back-to-back set_*_state calls -
// from a single thread or from concurrent threads - report a self-consistent
// "old -> new" transition at the call site without depending on the writer
// thread to drain events first. The race-fix UT below locks that property in.

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "log/event_types.hpp"
#include "log/state_registry.hpp"

namespace {

using hft::log::AppState;
using hft::log::ComponentId;
using hft::log::ComponentState;
using hft::log::StateRegistry;

TEST(StateRegistry, DefaultAppStateIsStarting) {
  StateRegistry reg;
  EXPECT_EQ(reg.app_state(), AppState::Starting);
  EXPECT_EQ(reg.app_code(), 0u);
}

TEST(StateRegistry, DefaultComponentStateIsDown) {
  StateRegistry reg;
  for (auto id : {ComponentId::App, ComponentId::Broker, ComponentId::Engine,
                  ComponentId::MarketData, ComponentId::Universe}) {
    EXPECT_EQ(reg.component_state(id), ComponentState::Down);
    EXPECT_EQ(reg.component_code(id), 0u);
  }
}

TEST(StateRegistry, ExchangeAppStateReturnsPriorAndStoresNew) {
  StateRegistry reg;
  const auto prev1 = reg.exchange_app_state(AppState::LoadingConfig, 100);
  EXPECT_EQ(prev1, AppState::Starting);  // default
  EXPECT_EQ(reg.app_state(), AppState::LoadingConfig);

  const auto prev2 = reg.exchange_app_state(AppState::Live, 200, /*code=*/7);
  EXPECT_EQ(prev2, AppState::LoadingConfig);
  EXPECT_EQ(reg.app_state(), AppState::Live);
  EXPECT_EQ(reg.app_code(), 7u);
}

TEST(StateRegistry, ExchangeComponentStateReturnsPriorAndStoresNew) {
  StateRegistry reg;
  const auto prev1 = reg.exchange_component_state(
      ComponentId::Broker, ComponentState::Starting, 100);
  EXPECT_EQ(prev1, ComponentState::Down);  // default
  EXPECT_EQ(reg.component_state(ComponentId::Broker), ComponentState::Starting);

  const auto prev2 = reg.exchange_component_state(
      ComponentId::Broker, ComponentState::Ready, 200, /*code=*/3);
  EXPECT_EQ(prev2, ComponentState::Starting);
  EXPECT_EQ(reg.component_state(ComponentId::Broker), ComponentState::Ready);
  EXPECT_EQ(reg.component_code(ComponentId::Broker), 3u);
}

TEST(StateRegistry, SetAppStateDelegatesToExchange) {
  StateRegistry reg;
  reg.set_app_state(AppState::ShuttingDown, 999, /*code=*/42);
  EXPECT_EQ(reg.app_state(), AppState::ShuttingDown);
  EXPECT_EQ(reg.app_code(), 42u);
}

TEST(StateRegistry, SetComponentStateDelegatesToExchange) {
  StateRegistry reg;
  reg.set_component_state(ComponentId::Engine, ComponentState::Error, 999,
                          /*code=*/77);
  EXPECT_EQ(reg.component_state(ComponentId::Engine), ComponentState::Error);
  EXPECT_EQ(reg.component_code(ComponentId::Engine), 77u);
}

TEST(StateRegistry, ComponentsAreIndependent) {
  StateRegistry reg;
  reg.set_component_state(ComponentId::Broker, ComponentState::Ready, 0);
  reg.set_component_state(ComponentId::Engine, ComponentState::Error, 0,
                          /*code=*/9);
  EXPECT_EQ(reg.component_state(ComponentId::Broker), ComponentState::Ready);
  EXPECT_EQ(reg.component_state(ComponentId::Engine), ComponentState::Error);
  EXPECT_EQ(reg.component_code(ComponentId::Broker), 0u);
  EXPECT_EQ(reg.component_code(ComponentId::Engine), 9u);
  // Untouched components remain at default.
  EXPECT_EQ(reg.component_state(ComponentId::Universe), ComponentState::Down);
}

// Race-fix UT: two threads concurrently exchange the same component's state.
// Each thread captures the prior value the registry returned. The atomic
// exchange must guarantee that exactly one of the two captured priors is the
// initial Down state and the other is whatever the first thread wrote. The
// final state must be one of the two values the threads wrote.
//
// This is the property that the producer-side exchange in LoggingState relies
// on so transitions are reported with a correct old_state even under back-to-
// back set_component_state calls.
TEST(StateRegistry, ConcurrentExchangeIsAtomic) {
  for (int trial = 0; trial < 64; ++trial) {
    StateRegistry reg;
    std::atomic<int> ready{0};
    std::atomic<int> start{0};

    ComponentState prior_a = ComponentState::Down;
    ComponentState prior_b = ComponentState::Down;

    std::thread ta([&] {
      ready.fetch_add(1);
      while (start.load(std::memory_order_acquire) == 0) {}
      prior_a = reg.exchange_component_state(ComponentId::Broker,
                                             ComponentState::Ready, 1);
    });
    std::thread tb([&] {
      ready.fetch_add(1);
      while (start.load(std::memory_order_acquire) == 0) {}
      prior_b = reg.exchange_component_state(ComponentId::Broker,
                                             ComponentState::Error, 2);
    });
    while (ready.load() < 2) {}
    start.store(1, std::memory_order_release);
    ta.join();
    tb.join();

    // Exactly one thread saw the initial Down value; the other saw what its
    // peer wrote. They must not both report Down (that would mean both threads
    // raced past the exchange without observing each other).
    const bool a_first = (prior_a == ComponentState::Down);
    const bool b_first = (prior_b == ComponentState::Down);
    EXPECT_NE(a_first, b_first) << "trial=" << trial;

    // The final state must be one of the two writes.
    const auto final_state = reg.component_state(ComponentId::Broker);
    EXPECT_TRUE(final_state == ComponentState::Ready ||
                final_state == ComponentState::Error)
        << "trial=" << trial;
  }
}

}  // namespace
