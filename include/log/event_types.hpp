#pragma once

#include <cstddef>
#include <cstdint>

// State-centric structured event types for the async logging pipeline.
//
// All event payloads are POD/trivially copyable so they can be moved across
// thread boundaries via the lock-free SPSC ring buffer without per-message
// heap allocation.

namespace hft::log {

enum class AppState : std::uint8_t {
  Starting = 0,
  LoadingConfig,
  InitializingLogging,
  ConnectingMarketData,
  ConnectingBroker,
  WaitingForServices,
  Live,
  Degraded,
  RiskOff,
  ShuttingDown,
  Fatal,
};

enum class ComponentState : std::uint8_t {
  Down = 0,
  Starting,
  Ready,
  Warning,
  Error,
};

enum class ComponentId : std::uint16_t {
  App = 0,
  MarketData,
  Broker,
  Risk,
  Strategy,
  Persistence,
  Logger,
  Universe,
  Engine,
  COUNT,
};

enum class EventType : std::uint16_t {
  AppStateChanged = 1,
  ComponentStateChanged,
  Heartbeat,
  WarningRaised,
  ErrorRaised,
  HealthSummary,
};

// Compact header shared by every event.  ts_ns is whatever monotonic
// nanosecond timestamp the producer captured at the call site.
struct EventHeader {
  std::uint64_t ts_ns;
  EventType type;
  std::uint16_t size;
  std::uint16_t producer_id;
  std::uint16_t component_id;
};

struct AppStateChangedEvent {
  EventHeader hdr;
  AppState old_state;
  AppState new_state;
  std::uint32_t code;
};

struct ComponentStateChangedEvent {
  EventHeader hdr;
  ComponentState old_state;
  ComponentState new_state;
  std::uint32_t code;
};

struct HeartbeatEvent {
  EventHeader hdr;
  std::uint64_t seq_no;
};

inline constexpr std::size_t kMessageMax = 96;

struct WarningRaisedEvent {
  EventHeader hdr;
  std::uint32_t code;
  char message[kMessageMax];
};

struct ErrorRaisedEvent {
  EventHeader hdr;
  std::uint32_t code;
  char message[kMessageMax];
};

const char* to_string(AppState s) noexcept;
const char* to_string(ComponentState s) noexcept;
const char* to_string(ComponentId id) noexcept;
const char* to_string(EventType t) noexcept;

}  // namespace hft::log
