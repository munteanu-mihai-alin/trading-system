#pragma once
#include <deque>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "broker/IBroker.hpp"
#include "config/LiveTradingConfig.hpp"
#include "engine/RankingEngine.hpp"

namespace hft {

// Thin live layer that consumes ranked stocks and routes top-K orders to a broker.
// Shadow portfolio, validation, benchmarking, and simulator stay in RankingEngine.
class LiveExecutionEngine {
 public:
  struct EntryOrderState {
    std::string symbol;
    double qty = 0.0;
    double limit = 0.0;
  };

  struct OpenPositionState {
    std::string symbol;
    double qty = 0.0;
    double entry_price = 0.0;
    double entry_ack_latency_ms = 1.0;
    int sell_order_id = 0;
    double sell_limit = 0.0;
    double sell_score = 0.0;
  };

 private:
  LiveTradingConfig cfg_;
  std::unique_ptr<IBroker> broker_;
  int next_order_id_ = 1;
  int orders_placed_ = 0;
  std::unordered_map<std::string, int> symbol_order_counts_;

  std::unordered_map<int, EntryOrderState> entry_orders_;
  std::unordered_map<std::string, OpenPositionState> open_positions_;
  std::unordered_map<int, std::string> exit_order_symbols_;
  std::unordered_set<std::string> depth_subscribed_symbols_;
  // Backtest-only decision-trace writer. Opened in the constructor when
  // cfg_.app.decision_log_path is non-empty; nullptr otherwise. Each buy
  // event emits one CSV row per ranked symbol.
  std::unique_ptr<std::ofstream> decision_log_;
  int next_decision_id_ = 0;
  // Optional log: every order state change (placed/filled/cancelled/
  // rejected) for both buys and sells. Realized PnL is derived from
  // this file end-of-run.
  std::unique_ptr<std::ofstream> order_log_;
  // Optional log: per-engine-step ranking snapshot for all symbols.
  // Same schema as decision_log_ but fires every step, not just on
  // buys. Heavier - see AppConfig::step_trace_log_path docs.
  std::unique_ptr<std::ofstream> step_trace_log_;
  int next_step_trace_id_ = 0;
  // Ring-buffer scope for step_trace emission. Mirror of
  // cfg_.app.step_trace_context_window cached here for the hot path.
  // 0 = legacy "log every step" behaviour; >0 = ring + trailing
  // policy. See AppConfig docs for the design rationale.
  int step_trace_context_window_ = 0;
  // Rolling buffer of the last N step snapshots (CSV chunks) that have
  // NOT yet been flushed to disk because no trade event fired. Drained
  // on trade event. Size bounded by step_trace_context_window_.
  std::deque<std::string> step_trace_ring_;
  // Steps remaining in the "post-event trailing" window. While > 0,
  // each step writes direct to disk (bypassing the ring) and decrements.
  // Re-armed (set to N) whenever a new trade event fires during
  // trailing - so overlapping events extend rather than duplicate
  // output.
  int step_trace_trailing_ = 0;
  // Per-step flag the broker call-sites flip when a buy or sell order
  // is placed this step. Read by the step_trace emitter at end of
  // step() and reset on the next step's entry. Independent of
  // step_trace_context_window_ being 0 - keeping it always-tracked
  // means tests can switch the window without restructuring state.
  bool step_trace_event_this_step_ = false;
  // Optional log: per-step L2 microstructure trace for held positions.
  // Written from route_exit_orders.
  std::unique_ptr<std::ofstream> l2_trace_log_;
  // Current engine step, set at top of step(t). Lets refresh_order_state
  // and other helpers tag emitted rows with the right step without
  // threading t through every signature.
  int current_step_t_ = 0;

  // ---- Daily-loss kill ALERT (AppConfig::daily_loss_kill_usd) ----
  // realized_pnl_ accumulates each exit Filled's
  //   (avg_fill_price - entry_price) * filled_qty
  // (commission is excluded; it's a small fraction of the kill threshold
  // and computing it precisely would couple us to the order-log column
  // schema).
  //
  // kill_alert_raised_ flips once when -session_pnl exceeds
  // cfg_.app.daily_loss_kill_usd. ALERT-ONLY: the engine writes a
  // single-line breach record to cfg_.app.daily_loss_kill_alert_path
  // (if set), logs a warning, and OTHERWISE CONTINUES NORMALLY.
  // Open orders are NOT cancelled and new orders are NOT refused.
  // Operator-in-the-loop response is intentional; we want to validate
  // the threshold reading under real conditions before promoting to a
  // hard kill. See AGENT_HANDOFF_LOG.md [2026-05-29] alert-only entry.
  //
  // Reset by start() so tests + multi-session runs start clean.
  double realized_pnl_ = 0.0;
  bool kill_alert_raised_ = false;

  // ---- User-triggered manual kill (AppConfig::kill_switch_trigger_path) ----
  // Polled at every step(): if the configured file exists, the engine
  // cancels every open entry + exit order and refuses to place new
  // orders for the rest of the session. Unlike kill_alert_raised_,
  // this IS destructive - it's the operator's explicit "stop trading
  // now" button. Reset by start() so tests can re-trigger between
  // runs, but the trigger FILE is NOT removed by the engine; the
  // operator owns its lifecycle.
  bool kill_switch_triggered_by_user_ = false;

  [[nodiscard]] bool can_route_order(const Stock& stock) const;
  [[nodiscard]] bool has_open_exposure(const std::string& symbol) const;
  [[nodiscard]] int open_exposure_symbol_count() const;
  [[nodiscard]] bool can_open_new_exposure() const;
  // Sum of entry-order qty*limit (pending) and open-position qty*entry_price.
  // Used to enforce account_budget across symbols.
  [[nodiscard]] double committed_notional() const;
  // floor(target_notional / limit_price) when target_notional > 0; else
  // falls back to order_qty / max_order_qty semantics. Returns 0 when the
  // order should be skipped (price too high, qty rounds to 0, etc.). The
  // target_notional argument lets the caller select the rule: by default
  // it is trade_notional, but the score-weighted path passes a per-symbol
  // share of account_budget instead.
  [[nodiscard]] double size_entry_qty(double limit_price,
                                      double target_notional) const;
  [[nodiscard]] bool sync_next_order_id_from_broker();
  [[nodiscard]] int portfolio_index_for_symbol(const std::string& symbol) const;
  [[nodiscard]] double allocated_daily_cost_per_share() const;
  [[nodiscard]] double estimate_round_trip_cost_per_share(
      double qty, double entry_price, double sell_price_estimate) const;
  void ensure_depth_subscription(const std::string& symbol, int ticker_id);
  // Rebuild open_positions_ from broker->query_positions() at startup so
  // an existing IBKR position (carried over from a prior session) isn't
  // orphaned. sell_limit and sell_order_id start at 0; route_exit_orders
  // populates them on the next step from the configured target. Returns
  // the number of positions recovered (0 when the broker has no positions
  // or when query_positions() is the IBroker default empty implementation).
  int reconcile_open_positions_from_broker();
  void refresh_order_state();
  void route_exit_orders();
  // Per-step drain of broker trade prints into Stock::hawkes, gated on
  // cfg_.app.hawkes_use_real_trades.
  void update_hawkes_from_trades();
  // Per-step hit-count windowing on s.mid, gated on hit_count_enabled.
  // Counts windows of `horizon` seconds where mid moved up by at least
  // `target_pct`, then sets s.score_tilt from the running count.
  void update_hit_count_tilt();
  // Writes one CSV row per ranked symbol on each buy event; no-op when
  // decision_log_ is null. The chosen_symbol arg tags which row is the
  // one that triggered this snapshot.
  void emit_decision_snapshot(int t, const std::string& chosen_symbol,
                              const std::string& gate);
  // Underlying writer used by both emit_decision_snapshot and the
  // per-step trace; factored so both files share the same 17-column
  // schema (16 ranking cols + ts_ns). decision_id is meaningful within
  // a single output file (each call increments). When chosen is empty,
  // no row is marked chosen=1. Takes std::ostream& (not std::ofstream&)
  // so the step_trace ring buffer can stage chunks through
  // std::ostringstream before flushing them in a batch.
  void write_ranking_snapshot_to(std::ostream& out, int decision_id, int t,
                                 const std::string& chosen_symbol,
                                 const std::string& gate);
  // Phase 1 of step_trace emission: serialize this step's snapshot and
  // either write it direct (legacy mode + trailing mode) or stash it
  // in the ring buffer awaiting a trade event (context-window mode,
  // pre-event). Idempotent when step_trace_log_ is null.
  void emit_step_trace_push_(int t);
  // Phase 2 of step_trace emission: if a trade event fired this step
  // AND we're in ring mode, flush the ring buffer (the "N before" part
  // of the user's design) and arm trailing for the next N steps. Called
  // at end of step() AFTER the entry / exit loops have had a chance to
  // set step_trace_event_this_step_. No-op in legacy (window=0) mode.
  void emit_step_trace_post_event_();
  // Open one of the CSV log files honouring cfg_.app.log_append_mode.
  // Writes the schema header when the file is fresh (truncate mode or
  // first-time append). Writes a session_start comment line in both
  // cases so restarts within the same file are clearly delimited.
  // Returns nullptr on open failure.
  std::unique_ptr<std::ofstream> open_log_(const std::string& path,
                                           const char* header,
                                           const char* log_kind);
  // Writes a session_end comment to each open log and flushes.
  void close_logs_with_session_end_();
  // Order-lifecycle event (placed/filled/cancelled/rejected). No-op
  // when order_log_ is null. Uses current_step_t_ for the step column.
  void emit_order_event(int order_id, const std::string& symbol,
                        const std::string& side, double qty, double limit,
                        const std::string& event, double filled_qty,
                        double remaining_qty, double avg_fill_price);
  // Per-step L2 trace row. No-op when l2_trace_log_ is null.
  void emit_l2_trace(const std::string& symbol, const L2Book& book,
                     double sell_limit, double sell_score);

  // Mark-to-market PnL across the session so far. Realized comes from
  // exits the engine has booked since start(); unrealized walks
  // open_positions_ and prices each against the current L2 best
  // bid/ask (falling back to L1 snapshot_top_of_book, then to
  // entry_price when neither is available so a missing book never
  // synthesises a false loss). Used by the daily-loss kill alert and
  // by tests that want to inspect mid-session state.
  [[nodiscard]] double compute_unrealized_pnl_mark_to_market() const;
  [[nodiscard]] double compute_session_pnl() const;
  // Raise the daily-loss kill ALERT: write one breach line to the
  // configured alert path (if any), log a warning, mark
  // kill_alert_raised_ so subsequent checks are no-ops. ALERT-ONLY -
  // no orders are cancelled, no orders are refused, the Engine
  // component is NOT moved to Error.
  void raise_daily_loss_kill_alert(double session_pnl);
  // Top-of-step hook that re-evaluates session PnL against
  // daily_loss_kill_usd and raises the alert when exceeded. Cheap
  // (O(open_positions) book snapshots). Disabled when the config knob
  // is <= 0.
  void check_daily_loss_kill_alert();
  // Reset the alert-file state at session start: if a path is
  // configured and a prior-session file is sitting there, remove it
  // so the file's presence post-start unambiguously means "this
  // session breached".
  void reset_daily_loss_kill_alert_file();

  // Poll the user-kill trigger path; if the file exists, run the
  // destructive kill (cancel orders, set the flag, log). Cheap
  // (one filesystem::exists call) so it's fine to call every step.
  // No-op when kill_switch_trigger_path is empty or the flag is
  // already set.
  void check_user_kill_switch();
  // Execute the manual-kill action. Reads the first line of the
  // trigger file (if any) for an operator-supplied reason, logs it,
  // cancels every entry + exit order in flight, and sets
  // kill_switch_triggered_by_user_. Idempotent - re-calls are
  // no-ops once the flag is set.
  void trigger_user_kill_switch();

 public:
  RankingEngine ranking;

  LiveExecutionEngine(LiveTradingConfig cfg, std::unique_ptr<IBroker> broker);

  bool start();
  void stop();

  void initialize_universe(int n_stocks);
  void subscribe_live_books(const std::vector<std::string>& symbols);
  void reconcile_broker_state();
  void step(int t);

  // Per-symbol target notional for the current step. Computed once per step
  // inside step() and also exposed publicly so tests can verify the
  // allocation rule (position_sizing_rule). Returns trade_notional for every
  // active symbol when the rule is "equal" or when score_weighted has no
  // positive score.
  [[nodiscard]] std::unordered_map<std::string, double>
  compute_per_symbol_notional() const;

  // Read-only access to the engine's view of currently open positions.
  // Used by main.cpp's end-of-run summary to compute realised vs unrealised
  // PnL.
  [[nodiscard]] const std::unordered_map<std::string, OpenPositionState>&
  open_positions() const {
    return open_positions_;
  }
  [[nodiscard]] int orders_placed() const { return orders_placed_; }

  // Read-only kill-alert accessors for tests + the end-of-run summary.
  // realized_pnl is the running sum of exit (avg_fill - entry) * filled
  // since the most recent start(); kill_alert_raised flips once and
  // never resets within a session.
  [[nodiscard]] double realized_pnl() const { return realized_pnl_; }
  [[nodiscard]] bool kill_alert_raised() const { return kill_alert_raised_; }
  [[nodiscard]] bool kill_switch_triggered_by_user() const {
    return kill_switch_triggered_by_user_;
  }

  // Test-only seam. Lets unit tests drive the kill-alert logic without
  // having to fake a full broker fill path to populate realized_pnl_.
  // Do NOT call from production code; the engine maintains its own
  // tally via refresh_order_state on exit Filled events.
  void inject_realized_pnl_for_test(double pnl) { realized_pnl_ = pnl; }
  // Force one round of the kill-alert evaluation against current state.
  // Equivalent to what step() does just after refresh_order_state.
  void check_daily_loss_kill_alert_for_test() { check_daily_loss_kill_alert(); }
  // Force one round of the user-kill poll. Test-only seam.
  void check_user_kill_switch_for_test() { check_user_kill_switch(); }
};

}  // namespace hft
