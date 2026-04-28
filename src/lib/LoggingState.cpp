#include "log/logging_state.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include "log/event_types.hpp"
#include "log/logging_service.hpp"

namespace hft::log {

namespace {

std::uint64_t now_ns() noexcept {
  using clock = std::chrono::steady_clock;
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          clock::now().time_since_epoch())
          .count());
}

std::uint16_t producer_id_for_this_thread() noexcept {
  thread_local static std::uint16_t id = []() {
    static std::atomic<std::uint16_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
  }();
  return id;
}

EventQueue& thread_queue() {
  return service().thread_queue();
}

void copy_message(char* dst, const char* src) noexcept {
  if (!src) {
    dst[0] = '\0';
    return;
  }
  std::size_t n = 0;
  while (n + 1 < kMessageMax && src[n] != '\0') {
    dst[n] = src[n];
    ++n;
  }
  dst[n] = '\0';
}

template <typename Event>
void push_event(const Event& ev) noexcept {
  RawEvent raw{};
  std::memcpy(raw.bytes, &ev, sizeof(ev));
  thread_queue().try_push(raw);
}

}  // namespace

void initialize_logging() {
  initialize_logging(LoggingService::Config{});
}

void initialize_logging(LoggingService::Config /*cfg*/) {
  // Lazy singleton; first call constructs and starts the writer thread.
  // Subsequent calls are idempotent (start() is a no-op once running).
  // The cfg argument is reserved for a future variant that swaps in a
  // configured singleton before first start.
  service().start();
}

void shutdown_logging() {
  if (!is_initialized())
    return;
  service().stop();
}

void set_app_state(AppState new_state, std::uint32_t code) noexcept {
  if (!is_initialized())
    return;
  AppStateChangedEvent ev{};
  ev.hdr.ts_ns = now_ns();
  ev.hdr.type = EventType::AppStateChanged;
  ev.hdr.size = sizeof(ev);
  ev.hdr.producer_id = producer_id_for_this_thread();
  ev.hdr.component_id = static_cast<std::uint16_t>(ComponentId::App);
  ev.old_state = service().registry().app_state();
  ev.new_state = new_state;
  ev.code = code;
  push_event(ev);
}

void set_component_state(ComponentId id, ComponentState new_state,
                         std::uint32_t code) noexcept {
  if (!is_initialized())
    return;
  ComponentStateChangedEvent ev{};
  ev.hdr.ts_ns = now_ns();
  ev.hdr.type = EventType::ComponentStateChanged;
  ev.hdr.size = sizeof(ev);
  ev.hdr.producer_id = producer_id_for_this_thread();
  ev.hdr.component_id = static_cast<std::uint16_t>(id);
  ev.old_state = service().registry().component_state(id);
  ev.new_state = new_state;
  ev.code = code;
  push_event(ev);
}

void heartbeat(ComponentId id) noexcept {
  if (!is_initialized())
    return;
  thread_local static std::uint64_t seq = 0;
  HeartbeatEvent ev{};
  ev.hdr.ts_ns = now_ns();
  ev.hdr.type = EventType::Heartbeat;
  ev.hdr.size = sizeof(ev);
  ev.hdr.producer_id = producer_id_for_this_thread();
  ev.hdr.component_id = static_cast<std::uint16_t>(id);
  ev.seq_no = ++seq;
  push_event(ev);
}

void raise_warning(ComponentId id, std::uint32_t code,
                   const char* msg) noexcept {
  if (!is_initialized())
    return;
  WarningRaisedEvent ev{};
  ev.hdr.ts_ns = now_ns();
  ev.hdr.type = EventType::WarningRaised;
  ev.hdr.size = sizeof(ev);
  ev.hdr.producer_id = producer_id_for_this_thread();
  ev.hdr.component_id = static_cast<std::uint16_t>(id);
  ev.code = code;
  copy_message(ev.message, msg);
  push_event(ev);
}

void raise_error(ComponentId id, std::uint32_t code, const char* msg) noexcept {
  if (!is_initialized())
    return;
  ErrorRaisedEvent ev{};
  ev.hdr.ts_ns = now_ns();
  ev.hdr.type = EventType::ErrorRaised;
  ev.hdr.size = sizeof(ev);
  ev.hdr.producer_id = producer_id_for_this_thread();
  ev.hdr.component_id = static_cast<std::uint16_t>(id);
  ev.code = code;
  copy_message(ev.message, msg);
  push_event(ev);
}

AppState current_app_state() noexcept {
  return is_initialized() ? service().registry().app_state()
                          : AppState::Starting;
}

ComponentState current_component_state(ComponentId id) noexcept {
  return is_initialized() ? service().registry().component_state(id)
                          : ComponentState::Down;
}

}  // namespace hft::log
