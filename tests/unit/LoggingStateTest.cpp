// Unit tests for the producer-side logging API in include/log/logging_state.hpp
// and src/lib/LoggingState.cpp.
//
// These tests exercise the public API surface and the underlying registry
// behavior. The LoggingService singleton is initialized once per test process
// (lazy and idempotent); writer-thread output is not asserted on here -
// LoggingServiceTest covers that. We assert on observable side effects via
// current_app_state() / current_component_state() and on no-crash semantics
// for messages.

#include <gtest/gtest.h>

#include <string>

#include "log/event_types.hpp"
#include "log/logging_service.hpp"
#include "log/logging_state.hpp"

namespace {

namespace hl = hft::log;

// One-time singleton init for this translation unit. Subsequent calls are
// no-ops because LoggingService::start() guards against double-start.
class LoggingStateFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    hl::LoggingService::Config cfg;
    cfg.enable_console_sink = false;  // keep test output clean
    cfg.log_file_path = "logs/test_logging_state.log";
    hl::initialize_logging(cfg);
  }
};

TEST_F(LoggingStateFixture, IsInitializedAfterInit) {
  EXPECT_TRUE(hl::is_initialized());
}

TEST_F(LoggingStateFixture, SetAppStateUpdatesRegistry) {
  hl::set_app_state(hl::AppState::LoadingConfig);
  EXPECT_EQ(hl::current_app_state(), hl::AppState::LoadingConfig);

  hl::set_app_state(hl::AppState::Live, /*code=*/0);
  EXPECT_EQ(hl::current_app_state(), hl::AppState::Live);
}

TEST_F(LoggingStateFixture, SetComponentStateUpdatesRegistry) {
  hl::set_component_state(hl::ComponentId::Broker, hl::ComponentState::Ready);
  EXPECT_EQ(hl::current_component_state(hl::ComponentId::Broker),
            hl::ComponentState::Ready);

  hl::set_component_state(hl::ComponentId::Broker, hl::ComponentState::Error,
                          /*code=*/42);
  EXPECT_EQ(hl::current_component_state(hl::ComponentId::Broker),
            hl::ComponentState::Error);
}

TEST_F(LoggingStateFixture, BackToBackSetComponentStateIsConsistent) {
  // Reproduces the producer-side ordering invariant the atomic-exchange race
  // fix in LoggingState/StateRegistry was added to guarantee. After two
  // back-to-back set_component_state calls, the registry must reflect the
  // latest write, not a stale earlier value.
  hl::set_component_state(hl::ComponentId::Engine, hl::ComponentState::Down);
  hl::set_component_state(hl::ComponentId::Engine,
                          hl::ComponentState::Starting);
  hl::set_component_state(hl::ComponentId::Engine, hl::ComponentState::Ready);
  EXPECT_EQ(hl::current_component_state(hl::ComponentId::Engine),
            hl::ComponentState::Ready);
}

TEST_F(LoggingStateFixture, HeartbeatDoesNotCrash) {
  // Heartbeat is fire-and-forget; the contract is just "must not crash and
  // must not change registry state".
  const auto before = hl::current_component_state(hl::ComponentId::Universe);
  hl::heartbeat(hl::ComponentId::Universe);
  EXPECT_EQ(hl::current_component_state(hl::ComponentId::Universe), before);
}

TEST_F(LoggingStateFixture, RaiseWarningAcceptsLongMessages) {
  // Producer must truncate at kMessageMax-1 and null-terminate. We can't
  // observe the on-the-wire bytes here without draining the SPSC queue, but
  // we can confirm the call completes without crashing for messages well
  // above the buffer cap and for the nullptr edge case.
  std::string long_msg(1024, 'x');
  hl::raise_warning(hl::ComponentId::Broker, /*code=*/1, long_msg.c_str());
  hl::raise_warning(hl::ComponentId::Broker, /*code=*/2, "short");
  hl::raise_warning(hl::ComponentId::Broker, /*code=*/3, nullptr);
}

TEST_F(LoggingStateFixture, RaiseErrorAcceptsLongMessages) {
  std::string long_msg(1024, 'y');
  hl::raise_error(hl::ComponentId::Engine, /*code=*/9, long_msg.c_str());
  hl::raise_error(hl::ComponentId::Engine, /*code=*/10, "short");
  hl::raise_error(hl::ComponentId::Engine, /*code=*/11, nullptr);
}

TEST_F(LoggingStateFixture, CurrentComponentStateMatchesLastWrite) {
  hl::set_component_state(hl::ComponentId::MarketData,
                          hl::ComponentState::Warning, /*code=*/5);
  EXPECT_EQ(hl::current_component_state(hl::ComponentId::MarketData),
            hl::ComponentState::Warning);
}

}  // namespace
