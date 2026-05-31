#pragma once

// Signal-based kill switches for the live engine.
//
// Two operator-initiated "stop" semantics, each backed by an
// std::atomic<bool> that LiveExecutionEngine polls every step():
//
//   USER_KILL (SIGUSR1)
//     Cancel every open entry + exit order. Refuse to place any new
//     order for the rest of the session. Open positions stay open --
//     the operator is freezing the trader, not the positions.
//
//   FORCE_LIQUIDATE (SIGUSR2)
//     Everything USER_KILL does PLUS: for each open position, place
//     a marketable sell at best_bid. "Save the portfolio at any
//     cost." Use when something is sufficiently wrong that holding
//     is riskier than taking the immediate exit prints.
//
// Both flags reset only on the next LiveExecutionEngine::start();
// the underlying atomics also clear there so a fresh session doesn't
// inherit a prior session's signal.
//
// Platform support:
//   - Linux: full implementation via sigaction(SIGUSR1, SIGUSR2).
//   - Windows (UCRT64 dev box): SIGUSR1/SIGUSR2 don't exist; the
//     install_kill_signal_handlers() function is a no-op. Tests use
//     the inject_*_for_test seams below, which work cross-platform
//     because they go through the same atomic the handlers flip.
//
// Operator quick-reference:
//   kill -USR1 $(pgrep hft_app)   # freeze trader
//   kill -USR2 $(pgrep hft_app)   # liquidate + freeze trader

namespace hft::kill_signals {

// Install SIGUSR1/SIGUSR2 handlers on Linux. No-op on Windows.
// Safe to call more than once; second call replaces the first.
// Called from LiveExecutionEngine::start().
void install_kill_signal_handlers();

// Atomic predicates the engine polls each step. Reading is cheap
// (relaxed atomic load). They stay sticky until the next start() so
// the engine doesn't miss a signal that arrives between steps.
[[nodiscard]] bool user_kill_requested();
[[nodiscard]] bool force_liquidate_requested();

// Clear both atomics. Called from LiveExecutionEngine::start() so a
// fresh session starts un-flagged. Tests use this for hermeticity.
void reset_for_session();

// Test-only seams. Flip the atomic without going through a real
// signal -- works cross-platform so the gtest suite is identical on
// Linux and Windows. Production code never calls these.
void inject_user_kill_for_test();
void inject_force_liquidate_for_test();

}  // namespace hft::kill_signals
