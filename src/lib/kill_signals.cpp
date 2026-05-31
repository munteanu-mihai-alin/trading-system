#include "engine/kill_signals.hpp"

#include <atomic>
#include <csignal>

namespace hft::kill_signals {

namespace {

// Two atomics, one per signal. We don't combine them because the
// engine's reaction to each is different (USER_KILL = freeze;
// FORCE_LIQUIDATE = freeze + sell open positions).
//
// std::atomic<bool> on x86_64 is lock-free for bool; reads in
// check_user_kill_switch() compile to a single mov.
std::atomic<bool> g_user_kill{false};
std::atomic<bool> g_force_liquidate{false};

#if defined(__linux__) || defined(__unix__) || defined(__APPLE__)
// Signal handlers MUST be async-signal-safe. std::atomic<bool>::store
// on every platform we care about IS signal-safe (lock-free on bool
// + relaxed-consistency store).
//
// Linux POSIX guarantees that SIGUSR1 and SIGUSR2 are NOT reserved
// for any libc/kernel use; they're free for application use.
extern "C" void on_sigusr1(int) noexcept {
  g_user_kill.store(true, std::memory_order_relaxed);
}

extern "C" void on_sigusr2(int) noexcept {
  g_force_liquidate.store(true, std::memory_order_relaxed);
}
#endif

}  // namespace

void install_kill_signal_handlers() {
#if defined(__linux__) || defined(__unix__) || defined(__APPLE__)
  // sigaction is preferred over signal() because it doesn't reset
  // the handler after firing (so a second SIGUSR1 after the first
  // still flips the atomic) and is portable across POSIX variants.
  struct sigaction sa{};
  sa.sa_handler = &on_sigusr1;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  sigaction(SIGUSR1, &sa, nullptr);

  sa.sa_handler = &on_sigusr2;
  sigaction(SIGUSR2, &sa, nullptr);
#else
  // Windows / UCRT64: SIGUSR1 / SIGUSR2 don't exist. The
  // inject_*_for_test seams below still work because they go
  // through the same atomic; production deployments use Linux.
#endif
}

bool user_kill_requested() {
  return g_user_kill.load(std::memory_order_relaxed);
}

bool force_liquidate_requested() {
  return g_force_liquidate.load(std::memory_order_relaxed);
}

void reset_for_session() {
  g_user_kill.store(false, std::memory_order_relaxed);
  g_force_liquidate.store(false, std::memory_order_relaxed);
}

void inject_user_kill_for_test() {
  g_user_kill.store(true, std::memory_order_relaxed);
}

void inject_force_liquidate_for_test() {
  g_force_liquidate.store(true, std::memory_order_relaxed);
}

}  // namespace hft::kill_signals
