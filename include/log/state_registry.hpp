#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "log/event_types.hpp"

namespace hft::log {

// In-memory snapshot of "what state is the application in right now?".
// Producer threads write component states with relaxed atomics; consumers
// (operator dashboards, health endpoints, periodic summary emitters) read
// them with the same loads.
struct ComponentStatus {
  std::atomic<std::uint8_t> state{
      static_cast<std::uint8_t>(ComponentState::Down)};
  std::atomic<std::uint64_t> last_update_ns{0};
  std::atomic<std::uint32_t> code{0};
};

class StateRegistry {
 public:
  StateRegistry() = default;
  StateRegistry(const StateRegistry&) = delete;
  StateRegistry& operator=(const StateRegistry&) = delete;

  void set_app_state(AppState s, std::uint64_t ts_ns,
                     std::uint32_t code = 0) noexcept {
    app_state_.store(static_cast<std::uint8_t>(s), std::memory_order_relaxed);
    app_last_update_ns_.store(ts_ns, std::memory_order_relaxed);
    app_code_.store(code, std::memory_order_relaxed);
  }

  void set_component_state(ComponentId id, ComponentState s,
                           std::uint64_t ts_ns,
                           std::uint32_t code = 0) noexcept {
    auto& c = components_[index_of(id)];
    c.state.store(static_cast<std::uint8_t>(s), std::memory_order_relaxed);
    c.last_update_ns.store(ts_ns, std::memory_order_relaxed);
    c.code.store(code, std::memory_order_relaxed);
  }

  AppState app_state() const noexcept {
    return static_cast<AppState>(app_state_.load(std::memory_order_relaxed));
  }

  std::uint32_t app_code() const noexcept {
    return app_code_.load(std::memory_order_relaxed);
  }

  ComponentState component_state(ComponentId id) const noexcept {
    return static_cast<ComponentState>(
        components_[index_of(id)].state.load(std::memory_order_relaxed));
  }

  std::uint32_t component_code(ComponentId id) const noexcept {
    return components_[index_of(id)].code.load(std::memory_order_relaxed);
  }

 private:
  static std::size_t index_of(ComponentId id) noexcept {
    auto i = static_cast<std::size_t>(id);
    constexpr auto count = static_cast<std::size_t>(ComponentId::COUNT);
    return i < count ? i : count - 1;
  }

  std::atomic<std::uint8_t> app_state_{
      static_cast<std::uint8_t>(AppState::Starting)};
  std::atomic<std::uint64_t> app_last_update_ns_{0};
  std::atomic<std::uint32_t> app_code_{0};

  std::array<ComponentStatus, static_cast<std::size_t>(ComponentId::COUNT)>
      components_{};
};

}  // namespace hft::log
