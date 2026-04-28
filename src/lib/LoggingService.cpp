#include "log/logging_service.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>

#include "log/event_types.hpp"
#include "log/state_registry.hpp"

#if __has_include(<spdlog/spdlog.h>)
#define HFT_LOG_HAS_SPDLOG 1
#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#else
#define HFT_LOG_HAS_SPDLOG 0
#endif

namespace hft::log {

namespace {

#if HFT_LOG_HAS_SPDLOG
std::shared_ptr<spdlog::logger> make_ops_logger(
    const LoggingService::Config& cfg) {
  constexpr std::size_t queue_size = 8192;
  constexpr std::size_t worker_count = 1;

  static std::once_flag thread_pool_once;
  std::call_once(thread_pool_once,
                 [&] { spdlog::init_thread_pool(queue_size, worker_count); });

  std::vector<spdlog::sink_ptr> sinks;
  if (cfg.enable_console_sink) {
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
  }
  if (!cfg.log_file_path.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(cfg.log_file_path).parent_path(), ec);
    sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(
        cfg.log_file_path, /*truncate=*/false));
  }

  auto logger = std::make_shared<spdlog::async_logger>(
      "hft_ops", sinks.begin(), sinks.end(), spdlog::thread_pool(),
      spdlog::async_overflow_policy::overrun_oldest);

  logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v");
  logger->set_level(spdlog::level::info);
  logger->flush_on(spdlog::level::warn);
  spdlog::register_logger(logger);
  return logger;
}
#endif

LoggingService* g_service = nullptr;
std::once_flag g_service_once;

}  // namespace

LoggingService::LoggingService() : LoggingService(Config{}) {}

LoggingService::LoggingService(Config cfg) : config_(std::move(cfg)) {}

LoggingService::~LoggingService() {
  stop();
}

void LoggingService::register_queue(EventQueue* q) {
  std::lock_guard<std::mutex> lock(queues_mutex_);
  queues_.push_back(q);
}

EventQueue& LoggingService::thread_queue() {
  thread_local EventQueue* tls_queue = nullptr;
  if (tls_queue)
    return *tls_queue;
  auto owned = std::make_unique<EventQueue>();
  EventQueue* raw = owned.get();
  {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    owned_queues_.push_back(std::move(owned));
    queues_.push_back(raw);
  }
  tls_queue = raw;
  return *raw;
}

void LoggingService::start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    return;
  }
  thread_ = std::thread([this] { run_(); });
}

void LoggingService::stop() {
  bool expected = true;
  if (!running_.compare_exchange_strong(expected, false)) {
    return;
  }
  if (thread_.joinable()) {
    thread_.join();
  }
#if HFT_LOG_HAS_SPDLOG
  spdlog::shutdown();
#endif
}

void LoggingService::run_() {
#if HFT_LOG_HAS_SPDLOG
  std::shared_ptr<spdlog::logger> ops = make_ops_logger(config_);
  ops->info("LoggingService started");
#endif

  auto next_summary =
      std::chrono::steady_clock::now() + config_.health_summary_interval;

  while (running_.load(std::memory_order_acquire)) {
    bool any_progress = false;
    std::vector<EventQueue*> snapshot;
    {
      std::lock_guard<std::mutex> lock(queues_mutex_);
      snapshot = queues_;
    }
    for (auto* q : snapshot) {
      RawEvent e;
      while (q->try_pop(e)) {
        any_progress = true;
        handle_event_(e);
      }
    }

    auto now = std::chrono::steady_clock::now();
    if (now >= next_summary) {
      emit_health_summary_();
      next_summary = now + config_.health_summary_interval;
    }

    if (!any_progress) {
      std::this_thread::sleep_for(config_.drain_idle_sleep);
    }
  }

  // Final drain on shutdown so we do not lose state transitions queued
  // immediately before stop().
  std::vector<EventQueue*> snapshot;
  {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    snapshot = queues_;
  }
  for (auto* q : snapshot) {
    RawEvent e;
    while (q->try_pop(e)) {
      handle_event_(e);
    }
  }

#if HFT_LOG_HAS_SPDLOG
  if (auto last = spdlog::get("hft_ops")) {
    last->info("LoggingService stopped");
    last->flush();
  }
#endif
}

void LoggingService::handle_event_(const RawEvent& raw) {
  EventHeader hdr{};
  std::memcpy(&hdr, raw.bytes, sizeof(EventHeader));

#if HFT_LOG_HAS_SPDLOG
  auto ops = spdlog::get("hft_ops");
#endif

  switch (hdr.type) {
    case EventType::AppStateChanged: {
      AppStateChangedEvent ev{};
      std::memcpy(&ev, raw.bytes, sizeof(ev));
      // Registry was already updated atomically at push time; only emit the
      // human-readable transition line here.
#if HFT_LOG_HAS_SPDLOG
      if (ops) {
        ops->info("APP {} -> {} code={}", to_string(ev.old_state),
                  to_string(ev.new_state), ev.code);
      }
#endif
      break;
    }
    case EventType::ComponentStateChanged: {
      ComponentStateChangedEvent ev{};
      std::memcpy(&ev, raw.bytes, sizeof(ev));
#if HFT_LOG_HAS_SPDLOG
      if (ops) {
        ops->info("{} {} -> {} code={}",
                  to_string(static_cast<ComponentId>(hdr.component_id)),
                  to_string(ev.old_state), to_string(ev.new_state), ev.code);
      }
#endif
      break;
    }
    case EventType::Heartbeat: {
      HeartbeatEvent ev{};
      std::memcpy(&ev, raw.bytes, sizeof(ev));
#if HFT_LOG_HAS_SPDLOG
      if (ops && (ev.seq_no % 60) == 0) {
        ops->debug("heartbeat {} seq={}",
                   to_string(static_cast<ComponentId>(hdr.component_id)),
                   ev.seq_no);
      }
#else
      (void)ev;
#endif
      break;
    }
    case EventType::WarningRaised: {
      WarningRaisedEvent ev{};
      std::memcpy(&ev, raw.bytes, sizeof(ev));
#if HFT_LOG_HAS_SPDLOG
      if (ops) {
        ops->warn("WARN {} code={} msg={}",
                  to_string(static_cast<ComponentId>(hdr.component_id)),
                  ev.code, ev.message);
      }
#endif
      break;
    }
    case EventType::ErrorRaised: {
      ErrorRaisedEvent ev{};
      std::memcpy(&ev, raw.bytes, sizeof(ev));
#if HFT_LOG_HAS_SPDLOG
      if (ops) {
        ops->error("ERROR {} code={} msg={}",
                   to_string(static_cast<ComponentId>(hdr.component_id)),
                   ev.code, ev.message);
      }
#endif
      break;
    }
    case EventType::HealthSummary:
    default:
      break;
  }
}

void LoggingService::emit_health_summary_() {
#if HFT_LOG_HAS_SPDLOG
  auto ops = spdlog::get("hft_ops");
  if (!ops)
    return;
  ops->info(
      "HEALTH app={} md={} broker={} risk={} strategy={} engine={} "
      "universe={}",
      to_string(registry_.app_state()),
      to_string(registry_.component_state(ComponentId::MarketData)),
      to_string(registry_.component_state(ComponentId::Broker)),
      to_string(registry_.component_state(ComponentId::Risk)),
      to_string(registry_.component_state(ComponentId::Strategy)),
      to_string(registry_.component_state(ComponentId::Engine)),
      to_string(registry_.component_state(ComponentId::Universe)));
#endif
}

LoggingService& service() {
  std::call_once(g_service_once, [] { g_service = new LoggingService(); });
  return *g_service;
}

bool is_initialized() noexcept {
  return g_service != nullptr;
}

// to_string helpers live with the implementation so the header stays
// allocation-free.
const char* to_string(AppState s) noexcept {
  switch (s) {
    case AppState::Starting:
      return "Starting";
    case AppState::LoadingConfig:
      return "LoadingConfig";
    case AppState::InitializingLogging:
      return "InitializingLogging";
    case AppState::ConnectingMarketData:
      return "ConnectingMarketData";
    case AppState::ConnectingBroker:
      return "ConnectingBroker";
    case AppState::WaitingForServices:
      return "WaitingForServices";
    case AppState::Live:
      return "Live";
    case AppState::Degraded:
      return "Degraded";
    case AppState::RiskOff:
      return "RiskOff";
    case AppState::ShuttingDown:
      return "ShuttingDown";
    case AppState::Fatal:
      return "Fatal";
  }
  return "?";
}

const char* to_string(ComponentState s) noexcept {
  switch (s) {
    case ComponentState::Down:
      return "Down";
    case ComponentState::Starting:
      return "Starting";
    case ComponentState::Ready:
      return "Ready";
    case ComponentState::Warning:
      return "Warning";
    case ComponentState::Error:
      return "Error";
  }
  return "?";
}

const char* to_string(ComponentId id) noexcept {
  switch (id) {
    case ComponentId::App:
      return "App";
    case ComponentId::MarketData:
      return "MarketData";
    case ComponentId::Broker:
      return "Broker";
    case ComponentId::Risk:
      return "Risk";
    case ComponentId::Strategy:
      return "Strategy";
    case ComponentId::Persistence:
      return "Persistence";
    case ComponentId::Logger:
      return "Logger";
    case ComponentId::Universe:
      return "Universe";
    case ComponentId::Engine:
      return "Engine";
    case ComponentId::COUNT:
      return "?";
  }
  return "?";
}

const char* to_string(EventType t) noexcept {
  switch (t) {
    case EventType::AppStateChanged:
      return "AppStateChanged";
    case EventType::ComponentStateChanged:
      return "ComponentStateChanged";
    case EventType::Heartbeat:
      return "Heartbeat";
    case EventType::WarningRaised:
      return "WarningRaised";
    case EventType::ErrorRaised:
      return "ErrorRaised";
    case EventType::HealthSummary:
      return "HealthSummary";
  }
  return "?";
}

}  // namespace hft::log
