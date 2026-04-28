#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "log/event_types.hpp"
#include "log/spsc_ring.hpp"
#include "log/state_registry.hpp"

namespace hft::log {

// Generic raw event buffer large enough for any event in event_types.hpp.
struct RawEvent {
  alignas(8) std::uint8_t bytes[160];
};

inline constexpr std::size_t kQueueSlots = 1024;
using EventQueue = SpscRing<RawEvent, kQueueSlots>;

// LoggingService owns the background writer thread.  It drains per-producer
// SPSC queues, decodes each event, mirrors important transitions through
// spdlog, and maintains the StateRegistry as the single source of truth for
// "current application state".
class LoggingService {
 public:
  struct Config {
    std::string log_file_path = "logs/app.log";
    std::chrono::milliseconds health_summary_interval{1000};
    std::chrono::milliseconds drain_idle_sleep{1};
    bool enable_console_sink = true;
  };

  LoggingService();
  explicit LoggingService(Config cfg);
  ~LoggingService();

  LoggingService(const LoggingService&) = delete;
  LoggingService& operator=(const LoggingService&) = delete;

  void start();
  void stop();

  // Acquire a per-thread queue.  The first call from a given thread creates
  // a new EventQueue owned by the service, registers it, and returns it; any
  // subsequent call from that thread returns the same queue.  This keeps the
  // single-producer / single-consumer contract on each ring.
  EventQueue& thread_queue();

  // Manually register an externally owned queue (e.g. for hot paths that
  // want a queue with a lifetime tied to a specific subsystem).  Pointer
  // must remain valid until stop().
  void register_queue(EventQueue* q);

  StateRegistry& registry() noexcept { return registry_; }
  const StateRegistry& registry() const noexcept { return registry_; }

 private:
  void run_();
  void handle_event_(const RawEvent& raw);
  void emit_health_summary_();

  Config config_;
  StateRegistry registry_;

  std::mutex queues_mutex_;
  std::vector<std::unique_ptr<EventQueue>> owned_queues_;
  std::vector<EventQueue*> queues_;

  std::atomic<bool> running_{false};
  std::thread thread_;
};

// Process-wide accessor.  initialize_logging() constructs the singleton if
// it does not exist yet and starts the writer thread.  shutdown_logging()
// stops it.  The pointer is owned by the implementation.
LoggingService& service();
bool is_initialized() noexcept;

}  // namespace hft::log
