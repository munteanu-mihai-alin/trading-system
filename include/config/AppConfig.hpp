#pragma once
#include <string>

namespace hft {

enum class BrokerMode { Paper, IBKRPaper, Live, Sim, DatabentoBacktest };

struct AppConfig {
  BrokerMode mode = BrokerMode::Paper;
  std::string host = "127.0.0.1";
  int paper_port = 7497;
  int live_port = 7496;
  int client_id = 1;
  int universe_size = 30;
  int top_k = 3;
  int steps = 500;
  bool order_enabled = true;
  double order_qty = 10.0;
  double max_order_qty = 10.0;
  double max_notional_per_order = 0.0;
  // Notional-driven sizing. When `trade_notional > 0`, the engine computes
  // entry qty as floor(trade_notional / limit_price) and ignores order_qty /
  // max_order_qty for that calc. `account_budget`, when > 0, caps the sum of
  // pending + open notional across all symbols. Both are in the order's
  // currency (USD for US equities).
  double trade_notional = 500.0;
  double account_budget = 1500.0;
  // Position-sizing rule. "equal" (default) uses trade_notional per active
  // symbol. "score_weighted" divides account_budget proportionally to each
  // active symbol's score (after ranking + tilt); symbols with non-positive
  // score get zero. "rank_weighted" allocates by rank position using
  // rank_weights below. score_weighted falls back to equal when total
  // active score is non-positive.
  std::string position_sizing_rule = "equal";
  // Ornstein-Uhlenbeck mean-reversion entry gate. Two parameterisations:
  //   - ou_halflife_seconds > 0 (preferred): dt-weighted EWMA where the
  //     half-life is in wall-clock seconds, consistent across symbols of
  //     different trade rates.
  //   - ou_window_size > 0 (legacy): sample-count EWMA where alpha =
  //     1/ou_window_size. Used only when ou_halflife_seconds <= 0.
  // Gate fires when ou_halflife_seconds > 0 OR ou_window_size > 0; the buy
  // path skips symbols where mid > ou.mu * (1 + ou_buy_threshold_pct).
  int ou_window_size = 0;
  double ou_halflife_seconds = 0.0;
  double ou_buy_threshold_pct = 0.0;
  // When true, LiveExecutionEngine subscribes to IBKR AllLast trade prints
  // for the live universe and drives the Stock::hawkes intensity from real
  // trade events (dt between consecutive trades on the symbol). When false,
  // Hawkes stays on RankingEngine's synthetic event clock. Default false
  // so existing tests and configs are unaffected.
  bool hawkes_use_real_trades = false;
  // Backtest-only "decision trace" log: when non-empty, the engine writes a
  // CSV row per ranked symbol on every buy decision, capturing the score,
  // score_tilt, Hawkes lambdas, hit_count, OU mu, mid, best_limit, and
  // whether the symbol was the one chosen. Default empty -> disabled, so
  // live trading and existing tests see no behavior change. Intended only
  // for backtest analysis; do not set in live configs.
  std::string decision_log_path;
  // Order-lifecycle CSV: when non-empty, the engine writes one row per
  // order state change (placed, filled, cancelled, rejected) covering
  // both buys and sells. Schema: step,order_id,symbol,side,qty,limit,
  // event,filled_qty,remaining_qty,avg_fill_price. Off by default; for
  // backtest and post-mortem analytics. Realized PnL is derived from
  // this file end-of-run, no separate fills.csv needed.
  std::string order_log_path;
  // Per-engine-step ranking snapshot. Same schema as decision_log_path
  // (16 cols) but fires every step, not just on buys. Useful to see how
  // scores, hawkes lambdas, OU mu, and hit_count evolve over the run.
  // Size estimate: ~130 bytes/row * 50 symbols/step * steps. A 4-day
  // backtest (~1560 steps) -> ~10 MB. Off by default. Intended for
  // backtest debugging; keep empty in live.
  std::string step_trace_log_path;
  // Per-step L2 snapshot for held positions. When non-empty, the engine
  // writes one row per open-position per step capturing best_bid/ask
  // + sizes, microprice, top-10 bid/ask volume, sell_limit, sell_score.
  // Schema: step,symbol,best_bid,bid_size,best_ask,ask_size,microprice,
  // bid_vol_top10,ask_vol_top10,sell_limit,sell_score. ~80 bytes/row.
  // For 3 held symbols across a 6-day backtest (~2340 steps): ~0.6 MB.
  // Off by default.
  std::string l2_trace_log_path;
  // Append vs truncate for the CSV logs (decision, order, step_trace,
  // l2_trace). When false (default), each engine start writes fresh
  // files - fine for backtest where reports/runs/<run_id>/ already
  // isolates sessions. When true, the engine appends to existing files
  // - intended for live trading where restarts within a day should
  // produce one continuous log with session boundary markers visible
  // (see session_start / session_end comment lines that the engine
  // writes at startup and shutdown).
  bool log_append_mode = false;
  // Optional human-readable label injected into the session_start /
  // session_end comment lines. Empty (default) just uses the mode
  // name. Useful when running multiple sessions per day and you want
  // to tag them ("morning_paper_smoke", "second_attempt_after_fix", ...).
  std::string run_label;
  // Optional override for the symbol universe. When non-empty, the
  // engine reads symbols (and optional company labels) from this file
  // at initialise() time INSTEAD of the hard-coded kSymbolCompanyList
  // in include/models/symbol_universe.hpp. File format: one symbol
  // per line, optional ",company" suffix. Blanks and lines starting
  // with '#' are skipped. Empty (default) keeps the legacy behaviour
  // of using kSymbolCompanyList. Useful for adversarial-window
  // backtests where the universe needs to drop symbols that didn't
  // exist yet (ARM, OKLO, SNDK, XPEV, HWM for pre-2024 windows).
  std::string symbol_universe_path;
  // Shadow-portfolio simulation in RankingEngine. When true, the engine
  // marks the next `shadow_top_k` symbols as `shadow_active` after the
  // top-k active ones, records synthetic per-step PnL for both real and
  // shadow slots into shadow_results.csv, and applies a cooldown when the
  // synthetic PnL is positive. The synthetic PnL has nothing to do with
  // real broker fills - it's a placeholder sine-wave for cooldown timing.
  // Default false: live trading, IBKR paper, and Databento backtest all
  // skip the synthetic block entirely. Code remains in place; only the
  // call sites are gated.
  bool shadow_enabled = false;
  // Synthetic FillModel inside RankingEngine. When true, ranking.step
  // runs the 8-candidate sweep against an internal Simulator order book
  // to estimate p_fill per candidate price; the score becomes
  // p_fill * hawkes.lambda. When false, the synthetic order book is
  // bypassed entirely: best_limit is set per `entry_limit_mode` below
  // (the broker decides fill against real L1/L2), and score is just
  // hawkes.lambda. Backtest mode should disable this to avoid the
  // O(N log N) sort_sides() cost per match_at_price that wedges long
  // runs - the real L2 inside the broker is the actual source of fill
  // probability. Default true keeps legacy HFT_TEST cases and the
  // paper-broker path unchanged.
  bool synthetic_fill_model = true;
  // Where RankingEngine sets `best_limit` when synthetic_fill_model is
  // off. Options:
  //   "mid"  - best_limit = s.mid. The buy crosses only if L1 ask drops
  //            to mid (passive entry). Stocks with non-trivial spread
  //            often never fill. Default for back-compat.
  //   "ask"  - best_limit = s.ask_price. Marketable - fills at the
  //            current ask whenever a TopOfBook is available. Use this
  //            for backtests where you want the buy decision to
  //            actually execute and exercise the sell-side path.
  std::string entry_limit_mode = "mid";
  // Bound the engine.step loop to broker's max replay-step count. When
  // true and the broker reports a positive max_replay_steps(), main.cpp
  // overrides cfg.steps to that value at startup. Prevents wasting CPU
  // on 98% of steps where L1/L2 are frozen past the data window's end.
  bool steps_auto_from_broker = false;
  // Hawkes intensity proxy driven by L1 mid changes (a tactical proxy
  // for trade-event arrivals in backtest, where no real trade prints
  // are available). When > 0, reconcile_broker_state fires a Hawkes
  // event each time s.mid moves by at least this many bps relative to
  // the last firing. Default 0 = disabled. The original
  // synthetic-event clock in RankingEngine continues to drive Hawkes
  // unless hawkes_use_real_trades=true (live IBKR) or this is > 0.
  double hawkes_mid_change_threshold_bps = 0.0;
  // Empirical "+target_pct hit-count" buy-side ranking tilt. Per-symbol
  // counter of historical price-increase windows that hit the target
  // return, multiplicatively tilting the ranking score. Disabled by
  // default; opt-in via hit_count_enabled. Horizon is the window length
  // in seconds; baseline is the hit-count divisor for the tilt; tilt
  // floors/ceilings clamp the resulting multiplier.
  bool hit_count_enabled = false;
  double hit_count_target_pct = 0.008;
  double hit_count_horizon_seconds = 60.0;
  double hit_count_baseline = 5.0;
  double hit_count_tilt_min = 1.0;
  double hit_count_tilt_max = 3.0;
  int max_open_symbols = 3;
  int max_orders_per_run = 0;
  int max_orders_per_symbol = 0;
  double target_profit_pct = 0.008;
  double min_sell_execution_score = 0.0;
  double commission_per_share = 0.005;
  // Per-order commission floor in dollars. IBKR Pro Fixed = $1.00; IBKR Pro
  // Tiered = $0.35. Default 1.0 preserves prior Fixed behaviour for
  // configs that don't explicitly set this. The cost model applies
  // max(commission_per_share * qty, commission_min_per_order) per leg.
  double commission_min_per_order = 1.0;
  double half_spread_cost = 0.0005;
  double impact_coefficient = 0.1;
  double assumed_daily_volume = 1'000'000.0;
  double daily_energy_kwh = 0.0;
  double energy_cost_per_kwh = 0.0;
  double daily_inflation_cost = 0.0;
  double expected_daily_shares = 1.0;
  // Daily-loss kill alert. Tracks the engine's running session PnL
  // (realized + unrealized, mark-to-market against the latest L2 best
  // bid / L1 mid) and when the loss against session-start equity exceeds
  // this dollar figure, the engine RAISES AN ALERT but does NOT take
  // any destructive action. Operator-in-the-loop: open orders are NOT
  // cancelled, new orders are NOT refused, the Engine component is NOT
  // flipped to Error. Instead a single-line breach record is written to
  // `daily_loss_kill_alert_path` (when set) and a warning is logged.
  //
  // The intent is observe-then-act: we want to see whether the
  // threshold fires correctly under real market conditions before
  // letting the engine auto-cancel. A future config knob (or split
  // mode) can promote this to a hard kill once we trust the readings.
  //
  // 0.0 (default) disables the check entirely so existing backtests,
  // sim runs, and paper smokes see no behaviour change. Set this in
  // live / IBKR-paper configs only.
  //
  // Example:
  //   daily_loss_kill_usd = 200.0
  //   daily_loss_kill_alert_path = logs/daily_loss_alert.txt
  // -> engine writes one line to that file when session PnL drops to
  //    -USD 200 relative to session start; subsequent breaches in the
  //    same session are no-ops (idempotent within session).
  //
  // See agent/AGENT_HANDOFF_LOG.md [2026-05-16] Live-trading prerequisites
  // sub-item #1.
  double daily_loss_kill_usd = 0.0;
  // Where the engine writes the alert when daily_loss_kill_usd is
  // breached. Empty (default) = do not write a file (the warning is
  // still logged through the normal logging component). Typically set
  // to something like `logs/daily_loss_alert_<run_label>.txt` so
  // monitoring scripts can `test -f` for it.
  //
  // File is truncated at every session start so prior-session alerts
  // don't masquerade as live ones. Within a session it's written
  // exactly once on the first breach (no growth across repeated step
  // calls). If you want a per-restart audit trail, point this at a
  // path that includes the timestamp or run_label.
  std::string daily_loss_kill_alert_path;

  // User-triggered manual kill switch. Path to a file the engine polls
  // at every step(): if it exists, the engine cancels every open
  // entry + exit order, sets kill_switch_triggered_by_user_, and
  // REFUSES TO PLACE NEW ORDERS for the rest of the session. The
  // file's first line (if present) is logged as the kill reason so
  // the operator can leave a breadcrumb like
  //     "panic - L2 looks bogus on KEYS"
  //
  // Empty (default) disables the gate so backtests / sim runs see no
  // overhead. To trigger from outside the process:
  //     touch $kill_switch_trigger_path           # bare flag
  //     echo "reason here" > $kill_switch_trigger_path
  //     ssh hetzner 'touch /tmp/hft_kill.flag'    # remote kill
  //
  // The file is NOT automatically removed when the engine triggers;
  // its presence is sticky so a restart of the engine with the file
  // still in place will re-trigger immediately. Operator must
  // explicitly `rm` it to clear, which is the intended audit trail.
  //
  // Distinct from daily_loss_kill_alert_path (which is OBSERVATIONAL
  // and where the engine writes); this path is INPUT and is where
  // the operator writes to kill the engine.
  std::string kill_switch_trigger_path;
  std::string databento_cache_dir = "data/databento";
  std::string databento_python = "python";
  std::string databento_l1_download_script = "scripts/local_l1_csv_provider.py";
  std::string databento_l2_download_script = "scripts/databento_download_l2.py";
  std::string databento_l1_dataset = "data/l1";
  std::string databento_l2_dataset = "XNAS.ITCH";
  std::string databento_l1_schema = "mbp-1";
  std::string databento_l2_schema = "mbp-10";
  std::string databento_start;
  std::string databento_end;

  [[nodiscard]] int port() const noexcept {
    if (mode == BrokerMode::Live)
      return live_port;
    return paper_port;
  }

  static AppConfig load_from_file(const std::string& path);
};

}  // namespace hft
