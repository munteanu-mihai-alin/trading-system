#pragma once

#include <cstdint>
#include <string>

#include "log/event_types.hpp"
#include "log/logging_service.hpp"

// Minimal API the rest of the project uses.  Producers call set_app_state /
// set_component_state from any thread; the calls are non-blocking and
// allocation-free on the fast path.

namespace hft::log {

void initialize_logging();
void initialize_logging(LoggingService::Config cfg);
void shutdown_logging();

void set_app_state(AppState new_state, std::uint32_t code = 0) noexcept;
void set_component_state(ComponentId id, ComponentState new_state,
                         std::uint32_t code = 0) noexcept;
void heartbeat(ComponentId id) noexcept;
void raise_warning(ComponentId id, std::uint32_t code,
                   const char* msg) noexcept;
void raise_error(ComponentId id, std::uint32_t code, const char* msg) noexcept;

// Read-side helpers, useful for health endpoints / tests.
AppState current_app_state() noexcept;
ComponentState current_component_state(ComponentId id) noexcept;

}  // namespace hft::log
