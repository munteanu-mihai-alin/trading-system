# Roadmap discussion — answers per question

Each item below quotes the user's question, then explains the current code, current behaviour, and the design choices for changing it. Numbering matches [`questions.md`](questions.md).

---

## 1. Restart-tolerant pickup of existing positions / orders + dynamic sell at current price

### What happens RIGHT NOW

- **Positions are picked up correctly.** `LiveExecutionEngine::start()` calls `broker_->query_positions()` (which on the live broker maps to IBKR `reqPositions`). For each returned `BrokerPosition{symbol, qty, avg_cost}` the engine populates `open_positions_[symbol]` with `entry_price = avg_cost`, `sell_order_id = 0`. See `src/lib/LiveExecutionEngine.cpp::reconcile_open_positions_from_broker`.
- **Sell limit is recomputed each step from `entry_price`, NOT from market.** In `route_exit_orders`:
  ```cpp
  const double gross_target = position.entry_price * (1.0 + profit_pct);
  const double cost_per_share = estimate_round_trip_cost_per_share(...);
  const double sell_limit = gross_target + cost_per_share;
  ```
  So the sell is placed at `entry_price * (1 + target_profit_pct) + cost_buffer`.
- **What happens if market has already moved way above target?** A limit order at a price BELOW the current bid fills immediately at the prevailing market mid. So if entry was $100, target is $100.80, and market is now $105, we place sell at $100.80 and IBKR fills it at $105 (the best bid the resting buyers are showing). We capture the FULL gain - the limit acts as a floor, not a ceiling.
- **What we MISS on a sudden spike:** the limit-becoming-marketable-on-place still leaves the trader holding the spread risk for one step. If between "submit" and "broker matches" the price drops back below entry+target, the order rests at its limit instead of capturing the spike. For HFT-grade fills the gap is microseconds; for our once-per-step routing it can be hundreds of ms.

### What's MISSING

- **Pickup of EXISTING open orders.** `query_positions` only returns positions. If a sell from a prior session is still working in IBKR when we restart, `route_exit_orders` sees `position.sell_order_id == 0` (no sell tracked in our state) and places a NEW sell on top of the old one - two sells for one position. This is documented as an Observation in `agent/ibkr_client_audit.md` ("No `reqAllOpenOrders` on startup").
- **Dynamic re-pricing as market spikes.** The current `sell_limit` is a function of `entry_price` only. If the market opens 5% above entry, our limit is still at entry+0.8%. The fill is correct (sells at market) but the LIMIT price logged in `orders.csv` is misleading - it reads as if we sold at +0.8% when we actually sold at +5%.

### Design choices to fix

**(a) Add `reqAllOpenOrders` + `reqExecutions` to startup reconcile.** Wire `IBroker::query_orders()` virtual + `IBKRClient::query_orders()` implementation. On startup, after `query_positions()`, also call `query_orders()` and for each open sell whose symbol matches an open_position, populate `position.sell_order_id = order.id` and `exit_order_symbols_[id] = symbol`. Then `route_exit_orders` skips because `sell_order_id != 0`. Net: ~80 LOC + the FakeIBKRTransport seed-orders helper for tests.

**(b) Adjust sell_limit upward when market is above target.** Two flavours:
   - **Marketable-at-bid clamp**: `sell_limit = max(entry * (1 + target_pct), current_bid - 1_tick)`. Captures the current spread while keeping the floor.
   - **Trailing stop**: store a high-water-mark and never lower the limit below `high_water * (1 - trail_pct)`. More state but captures larger spikes.
   The first is ~10 LOC and matches what marketable limits do anyway; the second is a strategy decision.

**(c) Refresh existing open sells when conditions change.** Today once placed, a sell is never modified. If market moves much higher, the resting sell at target eventually fills at much-higher market on a touch, but we'd be safer cancelling and re-submitting at a higher trailing level. Needs cancel-then-replace logic + an `orderModify` path (IBKR supports the modify shortcut to avoid a cancel race).

### Status
**Open.** Audit items #1 + position-reconcile already shipped; (a) (b) (c) above are all unshipped follow-ups.

---

## 2. Place sell at the same time as buy

### Why this is interesting

Today's flow is: `place buy` → wait for IBKR fill (~10-100ms typical) → next engine step sees position → `route_exit_orders` places sell. There's a "naked-long" window of one engine step between fill and sell. In a fast-moving market that can be enough to lose the edge.

### What IBKR already provides

- **Bracket orders**: parent buy + child take-profit sell + child stop-loss. When the parent fills, the children become active automatically. Only one of the children can fill; the other is auto-cancelled. The `transmit` flag on the parent gates whether the bracket activates on send (yes for our use).
- **OCO groups**: one-cancels-other. Two orders linked; either filling cancels the other. Useful for "sell at +0.8% OR -2%" stop+profit pairs.

Either of these would close the "naked-long" window completely - IBKR places the sell as soon as the buy fills, with no round-trip back to our engine.

### What changes in our code

- `OrderRequest` grows optional `bracket_take_profit_pct` and `bracket_stop_loss_pct` fields.
- `RealIBKRTransport::place_limit_order` constructs the parent + 1-2 children when those fields are set, sends them as a group with `parentId` on the children and `transmit=true` only on the last one.
- Engine no longer calls `route_exit_orders` for symbols whose bracket-sell is alive (track via `position.has_bracket_sell`).
- `OrderLifecycle` needs to handle child-order lifecycle events (a child Filled means the position is gone; a child Cancelled means the other child fired).

Effort: ~120 LOC + test against `MockIBKRTransport` recording the bracket structure. Strong recommend before any live (real-money) trading; medium recommend for paper.

### Concerns

- **Partial fills:** if the parent fills 5 of 10 shares, IBKR resizes the children to 5. We need to verify our engine handles that mid-bracket size change.
- **Symbols without depth subscription:** our current sell_limit uses L2 microstructure (sell_score from `compute_execution_score`). For bracket orders, we set the child limit at placement time, before we have L2. Either pre-subscribe L2 before placing the parent, or fall back to a simpler `entry * (1 + target)` for the bracket child.
- **Cancel ordering on day end:** if RTH closes with a bracket alive, the children stay GTC. Our day-end shutdown needs to cancel them explicitly.

### Status
**Open.** Worth a 1-2 day deep-dive when sub-step latency matters.

---

## 3. Allow multiple orders for top-scored stocks

### What happens now

- `cfg.app.max_orders_per_symbol` caps repeats per symbol (typically 1 for paper).
- `cfg.app.max_open_symbols` caps total open positions (typically 3).
- `cfg.app.top_k` selects the top-K symbols by score each step.
- If MKSI is rank 1 with score 50 and IBM is rank 2 with score 10 (much weaker), the engine still spends one of the 3 slots on IBM. Diversification by accident.

### What the user is proposing

Let high-conviction stocks consume MULTIPLE slots. E.g., if `score[rank_0] > 2 * score[rank_1]`, allow 2/3 of `top_k` to be the same stock.

### Tradeoffs

- **Concentration risk.** The yen window had IMOS down -89% intraday during the Aug 5 panic. Three slots all on IMOS would have been catastrophic during that print, even if the position recovered later. Our v6 baseline had `max_open_symbols=3` and reported a -89.7% deepest drawdown for a SINGLE position; concentrating 3x amplifies that.
- **Position sizing alternative.** Instead of multiple orders, scale the notional with the score. `target_notional = base * (1 + score_advantage_factor)`. Captures the high-conviction signal without breaching diversification.
- **Time-staggered building.** Place 1 share now, add another if the signal still ranks #1 N steps later. Kelly-style position building. Risk-managed without the "all in" downside.

### Recommended approach

1. **First**: measure how often the top-rank score is materially above rank 2. From a v6 baseline run we can compute `score[rank_0] / score[rank_1]` distribution. If it's almost always ~1.1x there's no signal to exploit; if it's 5-10x in adversarial windows it's worth changing.
2. **Then**: position sizing (option b) before multi-slot (option a). Lower risk, similar upside.

### Status
**Open.** Diagnostic first, then position sizing, then maybe multi-slot.

---

## 4. Step-by-step buy and sell

### One engine step (chronological)

This is what `LiveExecutionEngine::step(t)` does, in order. Source: `src/lib/LiveExecutionEngine.cpp::step`.

1. `current_step_t_ = t`. Reset `step_trace_event_this_step_ = false`.
2. `broker_->on_step(t)` - backtest replay advances; live broker no-op.
3. `reconcile_broker_state()`:
   - For each `items[i]`, read `broker_->snapshot_top_of_book(i+1)`. Update `s.mid`, `s.bid_price`, `s.ask_price`, `s.queue`. Fire Hawkes proxy event if mid moved beyond threshold.
   - `update_hawkes_from_trades()`: drain trades, update Hawkes lambda.
   - `update_hit_count_tilt()`: count target-pct moves, adjust `s.score_tilt`.
4. `refresh_order_state()`:
   - For each tracked entry order: read `OrderLifecycle::get(id)`. On `Filled`: erase from `entry_orders_`, upsert `open_positions_[symbol]` with `entry_price = avg_fill`, `entry_ack_latency_ms = lifecycle.ack_latency_ms()`. On `Cancelled` / `Rejected`: erase without touching positions.
   - For each tracked exit order: same. On `Filled`: erase from `exit_order_symbols_`, erase `open_positions_[symbol]`, accumulate `realized_pnl_ += (avg_fill - entry_price) * filled_qty`.
5. `check_daily_loss_kill_alert()`: re-evaluate `compute_session_pnl()` vs `daily_loss_kill_usd`. On breach: write breach line to `daily_loss_kill_alert_path`, log warning, set `kill_alert_raised_`. **Engine continues.**
6. `check_user_kill_switch()`: `std::filesystem::exists(kill_switch_trigger_path)`. If yes: cancel every entry + exit order, log warning, set `kill_switch_triggered_by_user_`. **Engine stops routing new orders for the rest of the session.**
7. `ranking.step(t)`: update scores from Hawkes + OU + hit_count. Sort `ranked_indices` by score.
8. `emit_step_trace_push_(t)`: serialise the ranking snapshot. Either ring-buffer push (quiet mode) or direct write (trailing mode). See `step_trace_context_window` config.
9. Early returns: if `kill_switch_triggered_by_user_` → refresh + return. If `!cfg.order_enabled` → return. If `!sync_next_order_id_from_broker()` → return.
10. `route_exit_orders()`: see Sell flow below.
11. Compute per-symbol target notional (equal or weighted-by-rank, depending on `cfg.weighted_sizing`).
12. **Entry loop**: iterate `ranked_indices` top-K. For each `Stock& s`:
    - Skip if `!s.active`.
    - Skip if `cfg.ou_buy_threshold_pct > 0` and `s.mid > s.ou.mu * (1 + threshold)` (OU mean-reversion gate).
    - `req.id = next_order_id_++; req.symbol = s.symbol; req.primary_exchange = primary_exchange_for(s.symbol); req.is_buy = true; req.limit = s.best_limit;`
    - Skip if `req.limit <= 0`.
    - `req.qty = size_entry_qty(req.limit, target_notional)`.
    - Skip on size 0 (price > notional) or `max_notional_per_order` breach.
    - Gate checks: `can_route_order(s)` (max_orders_per_run, max_orders_per_symbol, cooldown).
    - **Budget gate**: compute current open notional + this candidate; skip if > `account_budget`.
    - **max_open_symbols gate**: skip if already at limit.
    - `emit_decision_snapshot(t, s.symbol, gate_label)` → writes `decisions.csv`.
    - `broker_->place_limit_order(req)` → off to IBKR / backtest broker.
    - `emit_order_event(req.id, s.symbol, "buy", qty, limit, "placed", ...)` → `orders.csv`.
    - `entry_orders_[req.id] = EntryOrderState{...}`. `step_trace_event_this_step_ = true`.
    - `++orders_placed_; ++symbol_order_counts_[s.symbol]`.
13. `emit_step_trace_post_event_()`: if event fired this step, flush ring + arm trailing.
14. Heartbeat every 100 steps.

### Sell flow (inside `route_exit_orders`)

1. Loop over `open_positions_`.
2. Skip if `position.sell_order_id != 0` (sell already in flight).
3. `idx = portfolio_index_for_symbol(symbol)`. Skip if -1 (symbol not in universe - shouldn't happen post-reconcile).
4. `ensure_depth_subscription(symbol, kDepthTickerIdOffset + idx + 1)`: if L2 not yet subscribed, subscribe now.
5. `book = broker_->snapshot_book(depth_ticker_id)`. Skip if `!has_valid_top(book)` (no L2 yet).
6. Compute `sell_limit = entry_price * (1 + target_profit_pct) + cost_per_share`.
7. Compute `mid`, `queue_ahead = visible_ask_queue_ahead(book, sell_limit)`, `latency_ms`, `net_reward`, `loss`.
8. `sell_score = compute_execution_score(mid, sell_limit, directional_mu, hawkes_sell_lambda, queue_ahead, latency_ms, net_reward, loss)`.
9. Stash `position.sell_limit`, `position.sell_score` for diagnostics.
10. `emit_l2_trace(...)` → `l2_trace.csv` row, regardless of whether we submit.
11. Skip if `sell_score < min_sell_execution_score` (queue / odds too thin).
12. Compose `OrderRequest{is_buy=false, qty=position.qty, limit=sell_limit, ...}`.
13. `broker_->place_limit_order(req)`. `emit_order_event(..., "sell", "placed")`. `step_trace_event_this_step_ = true`.
14. `position.sell_order_id = req.id; exit_order_symbols_[req.id] = symbol`.

When IBKR reports Filled later: handled in step (4) above on the NEXT engine step.

### Status
Reference documentation, not a code change. Bake into a `docs/engine_flow.md` if useful for newcomers.

---

## 5. Average fill time for buy and for sell

### Backtest

`DatabentoBacktestBroker` is **synchronous-fill**: when `place_limit_order` is called, the broker checks the current L1/L2 snapshot and either marks Filled immediately (if marketable) or stages a resting order. There is no simulated transport latency, no queue-time-to-fill model. This is the open `#todo` recorded on 2026-05-26 (commit `e568ddb`).

So backtest "fill times" are 0 ms for marketable orders and "time-until-price-touches-limit" for resting orders. Yen v4 showed:
- 3 trips with `held_min = 0` → marketable on placement, instant fill at market mid.
- 3 trips with `held_min = 10468` (entire 7-trading-day window) → resting limit that took ~7 days to fire.

For live/paper we'd want to record actual fill latency.

### Live/paper (what to add)

Each `Filled` event in `orders.csv` already has its `ts_ns`. The matching `placed` event for the same `order_id` also has `ts_ns`. The difference is the fill latency.

Add a derived column or a separate summary in `plot_run.py`:
```
fill_latency_ms = ts_filled - ts_placed
```

Aggregates worth showing:
- mean / p50 / p90 / p99 for buys vs sells separately
- broken out by symbol (do illiquid names take longer?)
- broken out by marketable vs resting (was the limit aggressive enough?)

Effort: ~30 LOC in `scripts/plot_run.py` + tests against a fixture orders.csv. Pulls double duty - works for both backtest (instant) and paper (real).

### Status
**Open.** Useful diagnostic; ship before live trading.

---

## 6. Mobile app

### Scope

The user described a comprehensive monitoring + control surface:
- Run history list (paper + live + backtests)
- Per-run report with metrics, orders, fill times, latency histograms
- System health (memory, disk, CPU, IBKR Gateway connectivity)
- Push notifications
- AI-chat hook for failure diagnosis
- Ability to launch backtests from phone

### Architecture sketch

```
┌──────────────┐      ┌────────────────────┐      ┌──────────────────┐
│  iOS/Android │ HTTPS│  FastAPI on Hetzner │ FS  │  reports/runs/    │
│  app or PWA  │◀────▶│  (read-only at    )│◀───▶│  logs/            │
│              │      │   first, control   │     │  hft_app(running) │
│              │      │   later)           │     │  metrics endpoint │
└─────┬────────┘      └──────────┬─────────┘     └──────────────────┘
      │                          │
      ▼                          ▼
   ┌──────┐               ┌─────────────┐
   │ APNs/│               │ Slack/Telegram│ for ops alerts
   │ FCM  │               │ webhook      │
   └──────┘               └─────────────┘
```

### MVP scope (1-2 weeks)

1. **Backend service on Hetzner**: a small FastAPI app exposing
   - `GET /runs` - list (timestamp, label, n_trips, realized_pnl)
   - `GET /runs/{id}` - full metrics.json + decisions/orders CSVs as JSON
   - `GET /runs/{id}/report` - the existing markdown rendered to HTML, with the ratios block at the top per #10
   - `GET /system/health` - PID alive, disk usage, last heartbeat ts
   - `GET /system/credits/databento` - hits Databento `metadata.get_cost(... dummy ...)` to surface remaining budget
2. **PWA, not native**: easier to ship from this codebase. React + Tailwind + the FastAPI as the backend. Push notifications via the web Push API (works on Android; iOS supports as of 16.4).
3. **Read-only at first**: no test-launching from the phone yet. Launch from desktop, view from phone.

### Stretch (after MVP)

- Test-launch from phone (item #9). Adds auth + a job queue.
- AI-chat hook (Claude/Codex/Cursor). Routes through a webhook that takes a failed-run report + asks the LLM for a summary. The LLM gets read-only SSH to Hetzner via a dedicated key.
- Live trade feed (websocket) showing the per-step decision/orders as they happen.

### What we already have that the app can lean on

- `reports/runs/<id>/metrics.json` per run (after `plot_run.py`).
- `decisions.csv`, `orders.csv`, `l2_trace.csv`, `step_trace.csv` (small now with ring buffer) per run.
- `hft_status.sh` already prints health-like info (PID, log tail, L2 cache count, free RAM, local L1 count). The FastAPI endpoint can just call it via subprocess for free.
- The logging component already emits ComponentState transitions; we can read those from `stdout.log` for health.

### Status
**Big.** Plan in [`plan.md`](plan.md) treats it as Phase 2 (after live-paper smoke works). MVP backend is the natural first chunk; PWA can follow.

---

## 7. Daemon / systemd

**Note: the user wrote this BEFORE the alert-only daily-loss kill switch + the user-triggered file-based kill landed.** Several sub-items below are already addressed.

### Already shipped

- **Process kill** (umbrella's "killswitch: for killing the process"): user kill switch via `kill_switch_trigger_path`. Drop a file, engine cancels all open orders + refuses new. Operator-owned. Commit `3656a5c`.
- **Daily-loss alert** (a softer flavour of "killswitch for trading"): `daily_loss_kill_usd` + `daily_loss_kill_alert_path`. Writes a breach line, engine keeps trading. Commit `6fc7a62`.

### Still open

- **"Force selling in loss" trading kill.** Different semantics from both shipped flavours: when triggered, IMMEDIATELY market-sell every open position, then refuse new orders. Trigger source: the mobile app, or a daily-loss threshold escalation. New config: `force_liquidate_trigger_path` + `force_liquidate_market_orders=true`. ~50 LOC + tests.
- **systemd unit** (auto-restart on crash). One small `.service` file + an `[Install]` line. Restart=always, but with `RestartSec=30` so a crash-loop doesn't burn cycles. ~10 lines.
- **logrotate config** for `logs/cpp_backtest_*.log` and the future `logs/live_*.log`. Keep ~30 days compressed; the project's earlier budget was 100 GB rolling.
- **Process-health monitor.** A separate small daemon (Python or shell `while`) that:
  - Tails the engine's heartbeat (every 100 steps)
  - Watches `/proc/<pid>/status` for RSS / VmHWM
  - Watches `df` on `/mnt/HC_Volume_105581071`
  - On threshold breach, drops a file at the user-kill path AND posts to a webhook (Telegram bot, Slack incoming hook).

### Suggested layout

```
ops/
  systemd/
    hft_app.service           # engine itself
    hft_watchdog.service      # the health-monitor daemon
  logrotate/
    hft_app.conf
  watchdog/
    health_monitor.py
```

### Status
**Open.** The kill-switch portions are done; ops hardening (systemd + logrotate + watchdog) is the strong-recommend half of umbrella sub-item #4.

---

## 8. Backtest daemon

### What this would be

A separate supervisor process that:
- Holds a queue of pending backtest jobs (config path + label).
- Spawns `hft_app` for each job, waits for completion, archives, kicks off `plot_run.py`.
- Logs its own status to a `backtests.log`.
- Exposes a small status endpoint the mobile app can poll.

### Why it matters

Today `scripts/hft_backtest.sh` is a one-shot orchestrator with `--no-wait` quirks (the premature-archive bug we hit on yen v3). A long-lived daemon would:
- Make queueing multiple runs trivial.
- Survive my local laptop sleeping / disconnecting.
- Let the mobile app `POST /jobs` to enqueue.

### Status
**Open.** Worth doing alongside the mobile app backend (item #6) - they share infrastructure.

---

## 9. App-launched backtests

### From the user spec

- Pick config (with defaults)
- Pick start/end period
- Pick symbol universe (from a dropdown of saved files in `config/`)
- Show remaining Databento credits before launch (uses `metadata.get_cost`)
- Hook to LLM (Claude / Codex / Cursor) for failure investigation, with SSH-to-Hetzner access

### What we need

- Backend endpoint `POST /backtests/launch` taking the same args.
- A SAVED-config catalogue: list of `config.*.ini` files with a friendly label.
- A SAVED-universe catalogue: `config/symbols_*.txt` files.
- Databento client wrapper that calls `metadata.get_cost(...)` with a tiny dummy request to surface the credit balance. This is cost-free per Databento docs.
- For the LLM hook: a webhook that takes `{run_id, failure_log}` and forwards to one of three configured LLM endpoints. The LLM endpoints need SSH access scoped to a least-privilege user (read-only on logs + reports).

### Status
**Open.** Phase 3 - after mobile app backend MVP and after we trust paper trading enough to want remote-controlled experiments.

---

## 10. HTML report layout (ratios at top)

### What we have

`scripts/plot_run.py` produces a `metrics.md` per run. The "Summary" table is at top, has all the numeric ratios. After that comes round-trips, open positions, per-symbol plots, etc.

### What the user wants

Same data, but as HTML, with the ratios block visually prominent (top, possibly larger font, possibly a card-style layout).

### Change required

- `plot_run.py` learns a `--format=html` flag.
- Output goes through a small Jinja template (`templates/metrics.html.j2`).
- Ratios block uses a 2-3 column responsive layout with the key numbers (Sharpe, Sortino, n_trips, win_rate, realized_pnl, net_after_opp, deepest_drawdown_pct) in big tiles at the top.

Effort: ~80 LOC + template. No new dependencies (Jinja is already a transitive of pandas / matplotlib? Or add it).

### Status
**Quick win.** Suggest pairing with the mobile-app backend MVP - the same HTML is what the app would render.

---

## 11. Mid-session entry / pre-loaded ranking

### Why this matters

If we restart the engine at 11:02:35 (mid-session), the ranking's internal state (Hawkes intensities, OU mean estimates, hit_count) is empty. The engine would route orders based on near-zero scores until enough events accumulate (Hawkes typically wants ~hundreds of events per channel; OU wants enough mid observations).

In a backtest this is hidden by `cfg.steps` running from t=0. Live, we'd be making decisions on garbage for the first N minutes.

### Options

**(a) Wait N minutes before routing.** Add `cfg.app.warmup_minutes`. During warmup, engine subscribes + updates state but `cfg.app.order_enabled` is effectively false. After warmup elapses, normal routing. Cheapest. Drawback: lose some real-time edge during the warmup window.

**(b) Pre-load from cached prior day.** At startup, fast-forward the engine through the prior trading day's L1/L2 cache (we have this for the 10-day baseline already; we'd cache continuously in live). Hawkes lambda decays naturally so by the time the live session starts, intensities are at realistic levels. ~150 LOC for the fast-forward path + caching logic.

**(c) Serialise the engine state.** On graceful shutdown, write Hawkes/OU/hit_count to a `state.bin`. On startup, read it back. Doesn't survive a crash. Combines well with (a) as a fallback.

**(d) Hybrid: prefer (c), fall back to (b), fall back to (a).** Probably what we want long-term.

### Recommended first pass

**(a) is good enough for the first paper smoke.** It's 1 day of effort. Add `cfg.app.warmup_minutes = 15` to live configs; the engine subscribes + updates Hawkes/OU but skips all order placement until the wall clock passes the threshold. Tighten the recommendation later when we have actual event-rate measurements.

### Status
**Open.** First paper smoke needs at least (a). (b) and (c) become valuable when we move to multi-day live.

---

## 12. Improvements / monitoring suggestions

Things to monitor that the user didn't list explicitly but matter:

### Engine-side metrics
- **Order rejection rate** (broken out by IBKR error code 200 / 322 / 354 / etc.). Spikes here tell us something is wrong with our contract resolution or rate limiting.
- **Position drift vs IBKR's view.** Periodically diff `open_positions_` with `broker_->query_positions()`. Any mismatch = a fill we missed or a position we phantom-track.
- **L1 staleness per symbol.** `last_update_ts_ns` per symbol; alert if no update for >5s during RTH. Catches silent broker subscription drops.
- **Spread variance per symbol.** Per-symbol `(ask - bid) / mid` time series. Widening spreads = liquidity drying up = engine should slow down.
- **Cost-per-trade vs model.** From `commissionAndFeesReport` (currently no-op; should be wired). Diverging from `commission_per_share` config = real cost has drifted = re-calibrate.

### System-side metrics
- **Network RTT to IBKR Gateway.** A `ping` (well, a noop TWS API call) every 10s. Spikes correlate with rejection bursts.
- **L2 subscription health.** Count of symbols with at least one L2 update in the last minute. If < universe_size, somebody dropped silently.
- **Disk write rate to `reports/`.** Should be ~steady state. A sudden jump = a log file is misbehaving (we already had this happen).
- **Daily PnL distribution histogram.** Per session, per week. Helps spot regime change before the daily-loss alert fires.
- **`hft_app` RSS / VmHWM.** Memory leak detector. We saw 3.7 GB at peak; trending up over multi-day runs is a tell.

### Operational
- **IB Gateway connection uptime.** Reconnect attempts per hour. Each reconnect should re-replay subscriptions (open audit item #7 - not done yet).
- **Time since last successful order placement.** During RTH, if we go > 30 min with zero orders something might be wrong (or it's just quiet - alert as a question, not a panic).
- **Heartbeat freshness.** The engine writes `hl::heartbeat(Engine)` every 100 steps. If no heartbeat for >60s, alert.

### Status
Reference list. Wire each as we build out the watchdog daemon (#7) and the mobile-app health endpoint (#6).

---

## 13. Trading hours / per-symbol open windows

### What our universe looks like

The 50-symbol universe is dominated by US-listed names trading on NASDAQ (AAPL, NVDA, AMD, INTC, MU, CSCO, MKSI, etc.) and NYSE (IBM, HPE, RTX, LMT, LIN, NOC, ...). Plus a handful of ADRs / cross-listed names (TSM, ASML, NOK, TTE, UMC, XPEV, NIO, ASX, AMKR).

All of them have:
- **Regular Trading Hours (RTH)**: 9:30 AM – 4:00 PM ET (13:30 – 20:00 UTC summer; 14:30 – 21:00 UTC winter).
- **Pre-market**: 4:00 AM – 9:30 AM ET (limited liquidity, wider spreads).
- **After-hours**: 4:00 PM – 8:00 PM ET (similar).

ADRs technically have parallel trading windows in their home market (TSM on Taiwan from 21:00 ET to 04:30 ET, ASML on Amsterdam from 03:00 ET to 11:30 ET, etc.) but we route via the US ADR listing, so for us it's the US session.

So there's effectively **one trading window per day** for the engine: 09:30–16:00 ET, ~6.5 hours.

### Options

**(a) Keep the binary running 24/7.** Engine receives L1/L2 across pre-market + RTH + after-hours, but only places orders during RTH (gate via `cfg.app.order_enabled` + a built-in RTH check). Pros: no warm-up cost each morning (state survives overnight). Cons: ~17h/day burning CPU + memory for no trading.

**(b) Kill at close, relaunch at open.** systemd cron-style. Engine state lost; we need either pre-load (item #11 option (b)) or warm-up (item #11 option (a)). Pros: cheap. Cons: have to solve warm-up.

**(c) Hybrid: keep running but drop into low-power mode outside RTH.** Suspend ranking updates, keep L1/L2 subscriptions warm, no order routing. Decouples "state stays warm" from "no work happens".

### Recommended

**(c) is best long-term.** But for the first paper smoke, **(a) is simplest**: leave the engine running, add an RTH gate that suppresses order placement outside RTH, accept the modest CPU cost. Then add (c) when we have a monitoring story.

The RTH gate should be a small helper:
```cpp
bool is_rth_now(BrokerMode mode) {
  // Returns true if wall-clock is within 13:30 - 20:00 UTC, mon-fri,
  // not on a US market holiday. For backtest mode, always true (the
  // replay clock advances through whatever the data shows).
}
```

Hooks into `step()` right after `check_user_kill_switch` and before `ranking.step`. If `!is_rth_now(...) && cfg.app.mode != BrokerMode::DatabentoBacktest`, skip the order-placement loop (but still run reconcile + refresh so we'd see overnight fills land).

Plus a holiday calendar — US market closes on ~10 days/year. Hardcode them in a `models/market_calendar.hpp` + a test.

### Status
**Open.** Ship the RTH gate before any multi-day paper run. The cron / suspend variants are optimisation.

---

## Cross-cutting suggestions (not in the original 13)

- **Per-run `report.html` symlink / index page** that the mobile app and the local browser can both load. Just `reports/runs/<id>/report.html` next to `metrics.md`.
- **`scripts/end_of_day.sh`** that runs at 16:01 ET: archive the day's loose `reports/*.csv` to a daily folder, run plot_run, post the summary to a webhook. Becomes the "what happened today" notification.
- **Order-id namespacing per session.** Today the engine uses `next_order_id_` starting from 1 (or from IBKR's nextValidId on real). For multi-day live, IBKR's auto-incremented id is already unique session-to-session; just confirm `sync_next_order_id_from_broker` handles the jump.
