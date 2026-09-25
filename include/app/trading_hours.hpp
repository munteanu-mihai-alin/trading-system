#pragma once

#include <chrono>

#include "config/AppConfig.hpp"

// Is the market actually open right now?
//
// Freshness (see Chronos2ExecutionEngine::reconcile_broker_state) asks
// whether a quote is recent. That is a different question from whether
// we should be trading on it: after-hours quotes are perfectly fresh,
// and also thin and wide, which is not what this strategy was designed
// or backtested for. Verified live at 18:31 ET on a Friday -- books
// were updating and md read Ready, well after the 16:00 close.
//
// Normal operation is bounded by the RTH systemd timers, so this is
// defence in depth for the paths that bypass them: the start timer's
// Persistent=true fires a missed run after a reboot, and an operator
// can always `systemctl start hft_app@paper` at any hour.
//
// Market holidays are deliberately NOT enumerated here. On a holiday
// no quotes flow, so the freshness guard already blocks entries -- the
// two guards compose, and neither needs a calendar that would rot.
// The cost of that choice is that a holiday looks like a dead feed
// rather than a closed market, which is fine for gating but would be
// misleading if ever surfaced to a human as a reason.

namespace hft {

// NYSE regular session, America/New_York. Half-days (13:00 closes) are
// not modelled: trading the extra hours on those few days is a minor
// wrong, and the alternative is the exchange calendar this header
// exists to avoid.
inline constexpr std::chrono::minutes kRthOpen =
    std::chrono::hours{9} + std::chrono::minutes{30};
inline constexpr std::chrono::minutes kRthClose = std::chrono::hours{16};

// True when tp falls inside the NYSE regular session.
//
// Fails CLOSED: if the tz database cannot be read the answer is "not
// open", so a broken environment refuses to trade rather than trading
// at an arbitrary hour. Caller logs the distinction.
[[nodiscard]] inline bool is_within_rth(
    std::chrono::system_clock::time_point tp) {
  using namespace std::chrono;
  try {
    const auto* tz = locate_zone("America/New_York");
    const zoned_time zt{tz, tp};
    const auto local = zt.get_local_time();
    const auto midnight = floor<days>(local);
    const weekday wd{midnight};
    if (wd == Saturday || wd == Sunday)
      return false;
    const auto since_midnight = local - midnight;
    return since_midnight >= kRthOpen && since_midnight < kRthClose;
  } catch (const std::exception&) {
    return false;
  }
}

// Whether the RTH gate applies at all.
//
// Backtests must never be gated: they replay historical sessions at
// whatever wall-clock time the run happens to execute, so asking "is
// the market open now" is meaningless and would refuse every entry.
// Same shape as effective_step_interval_ms -- the mode check lives
// here rather than in config so a backtest cannot inherit a live
// config and silently trade nothing.
[[nodiscard]] inline bool rth_gate_applies(bool cfg_require_rth,
                                           BrokerMode mode) {
  if (mode == BrokerMode::DatabentoBacktest)
    return false;
  if (mode == BrokerMode::Sim)
    return false;
  return cfg_require_rth;
}

}  // namespace hft
