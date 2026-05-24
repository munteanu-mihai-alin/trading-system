# IBKRClient Code-Path Audit (Sub-item 8 of Live-Trading Prerequisites)

Audit date: 2026-05-21. Source: `bb6e421` ish (cross-wire + L2-pacing +
plot_run + cost-calibration + position-reconciliation already landed;
this audit is for the IBKRClient-specific gaps that remain).

## Purpose

Identify gaps between what `LiveExecutionEngine` calls on `IBroker` and
what `IBKRClient` actually implements. Output: a triage list with
**hard blockers** (paper trading will misbehave), **strong-recommend
fixes** (paper trading will work but is fragile), and **observations**
(non-blocking, worth knowing).

## Methodology

Walked every `broker_->X(...)` call site in `LiveExecutionEngine.cpp`,
matched to its `IBKRClient::X` implementation and the underlying
`RealIBKRTransport` TWS API call. Tagged each path as
**Wired**, **Partial**, **Stub**, or **Missing**.

---

## Path-by-path findings

| Engine call | IBKRClient | RealIBKRTransport / TWS API | Status |
|---|---|---|---|
| `connect(host,port,client_id)` | sets state, calls transport | `client_.eConnect()` + EReader start | **Wired** |
| `disconnect()` | joins reader, transport->disconnect | `client_.eDisconnect()`, EReader stop | **Wired** |
| `is_connected()` | passes through | `connected_ && client_.isConnected()` | **Wired** |
| `place_limit_order(req)` | tracks send_ts, calls transport | `client_.placeOrder(req.id, contract, order)` STK/SMART/USD | **Partial** — see #1 |
| `cancel_order(id)` | passes through | `client_.cancelOrder(id, OrderCancel{})` | **Wired** |
| `start_event_loop()` | spawns reader thread | `transport_->pump_once()` loop | **Wired** |
| `stop_event_loop()` | joins reader thread | — | **Wired** |
| `subscribe_top_of_book(req)` | passes through | `client_.reqMktData(ticker_id, contract, "", false, false, TagValueListSPtr())` | **Partial** — see #2, #3 |
| `subscribe_market_depth(req)` | passes through | `client_.reqMktDepth(ticker_id, contract, depth, false, TagValueListSPtr())` | **Partial** — see #2 |
| `subscribe_trades(req)` | passes through | `client_.reqTickByTickData(ticker_id, contract, "AllLast", 0, false)` | **Wired** |
| `drain_trades(ticker_id)` | mutex-protected FIFO swap | — | **Wired** |
| `snapshot_top_of_book(ticker_id)` | reads top_books_, falls back to L2 best | — | **Wired** |
| `snapshot_book(ticker_id)` | reads books_ | — | **Wired** |
| `order_lifecycle()` | returns &lifecycle_ | — | **Wired** |
| `ack_latency_ms(id)` | reads cache populated on Submitted | — | **Partial** — see #4 |
| **`query_positions()`** | **inherits IBroker default (empty)** | **no reqPositions wiring** | **Missing** — see #5 |
| `max_replay_steps()` | default 0 (live shouldn't override) | — | **Wired** (not applicable to live) |
| `sync_next_order_id_from_broker()` (engine internal) | dynamic_casts to IBKRClient, reads next_valid_order_id_ | nextValidId callback populates | **Partial** — see #6 |
| Reconnect (`reconnect_once`) | ConnectionSupervisor backoff + re-connect | — | **Partial** — see #7 |

### IBKRCallbacks → IBKRClient method handling

| Callback | IBKRClient handler | Notes |
|---|---|---|
| `on_order_status(id, status, filled, remaining, avg)` | forwards to `lifecycle_`; updates ack_latency_ms_cache_ on Submitted/PreSubmitted | **Wired** |
| `on_market_depth_update(ticker, pos, op, side, price, size)` | mutex-guarded write into `books_` | **Wired** |
| `on_top_of_book_price(ticker, is_bid, price)` | mutex-guarded write into `top_books_` | **Wired** (handles BID/ASK/DELAYED variants in transport's tickPrice) |
| `on_top_of_book_size(ticker, is_bid, size)` | mutex-guarded write into `top_books_` | **Wired** |
| `on_trade(ticker, price, qty, exch_ts_ns)` | append to per-ticker FIFO | **Wired** |
| `on_next_valid_id(id)` | mutex-guarded store | **Wired** |
| `on_error(IBKRError)` | append to errors_ vector | **Partial** — see #8 |
| `on_connection_closed()` | raise_error + Broker→Error state | **Wired** |

### Lifecycle status mapping (OrderLifecycle.hpp)

| TWS status string | mapped to OrderLifecycleStatus | Engine treats as terminal? |
|---|---|---|
| `"Submitted"`, `"PreSubmitted"` | `Submitted` | No |
| `"Filled"` | `Filled` | **Yes** |
| `"PartiallyFilled"`, anything with `filled>0 && remaining>0` | `PartiallyFilled` | **No** — see #9 |
| `"Cancelled"`, `"ApiCancelled"` | `Cancelled` | **Yes** |
| `"Inactive"` | `Rejected` | **Yes** |
| anything else | depends on filled/remaining | — |

---

## Issues found, prioritised

### Hard blockers (paper trading will misbehave)

#### #1. Order Contract is hardcoded to SMART/USD/STK only
[RealIBKRTransport.cpp:75-91](src/lib/RealIBKRTransport.cpp:75) builds every
Contract with `secType="STK"`, `exchange="SMART"`, `currency="USD"`, no
`primaryExchange`. For dual-listed or NYSE-vs-NASDAQ ambiguous symbols,
SMART routing can fail to resolve (this is what blocked PSTG in the L1
backfill). Same code path is used for L1 + L2 subscribes.

**Fix**: thread an optional `primary_exchange` field through
`OrderRequest` / `TopOfBookRequest` / `MarketDepthRequest`, populated
from a per-symbol map. Sub-item 9 (symbol-contract audit) will identify
which symbols need it.

#### #5. query_positions() not implemented
The position-reconciliation hook landed in `a734313` but IBKRClient
inherits the IBroker default (returns empty). Means restart-tolerance
is structurally available but inert for live/paper.

**Fix**: in IBKRClient, override `query_positions()`. The flow:
1. Add `IBKRCallbacks::on_position(account, symbol, qty, avg_cost)` and
   `on_position_end()` virtual methods.
2. Add `RealIBKRTransport::position(...)` override that translates
   Decimal → double and forwards. Also `positionEnd()`.
3. IBKRClient stores positions in a member vector under a mutex, with
   a condition variable signalled by on_position_end.
4. `query_positions()` calls `transport_->reqPositions()` (new
   IBKRTransport virtual method), blocks on the CV until end, returns
   the vector, then sends `cancelPositions()` so we don't keep getting
   updates.

Estimated effort: ~80-120 LOC across the 3 files + 1 unit test
using FakeIBKRTransport.

### Strong-recommend (paper will work but is fragile)

#### #2. No `reqMarketDataType()` call → silent fallback to delayed data
With only the free "US Real-Time Non Consolidated Streaming Quotes"
subscription active, calls to `reqMktData` will deliver delayed ticks
(15-min lag) for symbols outside that feed's coverage. Engine receives
`DELAYED_BID`/`DELAYED_ASK` ticks (handled in `tickPrice`) but operates
as if they were real-time — every decision is 15 minutes stale.

**Fix**: in `RealIBKRTransport::connect()`, after eConnect succeeds,
call `client_.reqMarketDataType(1)` to force real-time-or-error.
IBKR returns error 10167 ("requested market data is not subscribed")
which surfaces via `on_error` — let the engine catch that and surface
to the user as "subscribe to NASDAQ TotalView/NYSE OpenBook before
running live".

#### #3. `reqMktData` is called without `genericTickList`
Some downstream consumers may want LAST_PRICE, LAST_SIZE, or RT_VOLUME.
Today only BID/ASK/BID_SIZE/ASK_SIZE arrive via `tickPrice`/`tickSize`.
For the current strategy this is fine (`s.mid` derived from bid/ask).
But if `hawkes_use_real_trades=true` is set without an `AllLast`
subscription, we'd silently get nothing.

**Fix**: when `hawkes_use_real_trades=true`, ensure `subscribe_trades`
is also called (it's already a separate call). Document the dependency
in a comment near the config flag. Optional: add `"233"` (RT_VOLUME)
to `genericTickList` for downstream coverage.

#### #4. `send_ts_` and `ack_latency_ms_cache_` are not mutex-protected
[IBKRClient.cpp:62,71,205](src/lib/IBKRClient.cpp:62) — these unordered_maps
are written by the engine thread (place_limit_order) and the reader
thread (on_order_status). They live under no mutex. Concurrent writes
will eventually rehash and crash, or silently corrupt values.

**Fix**: wrap both maps under `event_mutex_` (already exists for
`next_valid_order_id_` + `errors_`). Holds for a microsecond per call,
no realistic perf impact.

#### #6. nextValidId is fire-once on connect; no explicit reqIds refresh
On clean session start the nextValidId callback fires from IBKR
automatically. If the cached value gets out of sync (multi-client
account, manual order entry in TWS during a session), `next_order_id_`
could collide with an existing IBKR order ID → reject.

**Fix**: add an explicit `reqIds(-1)` call in `IBKRClient::connect()`
after the transport's connect succeeds. The reqId arg is ignored by
TWS in recent API versions; this just forces a fresh nextValidId.
Also wire `sync_next_order_id_from_broker()` to call reqIds periodically
(every N steps) or after any error 322 (duplicate order id).

#### #7. Reconnect doesn't replay subscriptions
`reconnect_once()` re-calls `connect()` but doesn't re-issue
`reqMktData`/`reqMktDepth`/`reqTickByTickData` for the symbols the
engine had previously subscribed. After a Gateway flap the engine
runs blind — `top_books_` and `books_` are still populated from before
the disconnect but nothing updates them.

**Fix**: track active subscriptions in IBKRClient as a `std::vector<std::variant<TopOfBookRequest, MarketDepthRequest, TopOfBookRequest_trades>>`
and replay them in a `reissue_subscriptions()` helper called from the
reconnect path. Also need to clear the stale `top_books_` / `books_`
on reconnect so we don't act on pre-flap data.

#### #8. Error callback collects but engine doesn't react
`on_error` appends to `errors_` vector with no consumption. Specific
error codes that the engine should react to:

| Code | Meaning | Suggested action |
|---|---|---|
| 200 | No security definition | Drop symbol from active universe (log + warn) |
| 322 | Duplicate order ID | Call reqIds; retry place_limit_order |
| 354 | Requested market data is not subscribed | Surface to user before run starts; halt if critical |
| 1100 | Connectivity lost | Stop accepting new orders; rely on reconnect |
| 1101/1102 | Connectivity restored | Replay subscriptions (see #7) |
| 2103/2104/2105/2106 | Connection status updates (data farm) | Log only; affects which symbols stream |
| 10167 | Requested market data not subscribed (delayed not available) | Same as 354 |

**Fix**: add an `IBKRClient::drain_errors() -> vector<IBKRError>` that
the engine polls each step, with a switch on `code` that takes the
appropriate action. Some codes should also surface via
`hl::raise_warning`/`hl::raise_error` to make the operational log
readable.

### Observations (non-blocking, worth knowing)

#### Lifecycle PartiallyFilled is captured but not acted on
A partial fill of an entry order updates the lifecycle book to
`PartiallyFilled`. `refresh_order_state` (in LiveExecutionEngine.cpp:605)
only branches on `Filled` for entry orders. A symbol that fills 5 of
10 shares then gets cancelled would leave the engine thinking nothing
happened, while 5 shares actually sit in the account. Combined with
position reconciliation (sub-item 10 / a734313), this gets papered over
on the NEXT engine restart but mid-session is wrong.

**Recommend**: extend `refresh_order_state` to upsert
`open_positions_[symbol]` on every `PartiallyFilled` event with the
running filled qty, NOT just on terminal `Filled`. Sell-side: route
exits on the running qty.

#### Tiered pricing not modeled in IBKRClient
The cost model lives in the engine (commission_per_share +
commission_min_per_order). IBKR delivers actual commissions per fill via
`commissionAndFeesReport` callback (currently a no-op in
RealIBKRTransport). For paper this is moot (paper doesn't charge), but
for live trading the actual realized cost should override the model.

**Recommend**: wire `commissionAndFeesReport` → IBKRCallbacks → engine
side, accumulate realized commission per-order, expose via
`order_lifecycle()` for plot_run.py and metrics. Out of scope for
sub-item 8.

#### No `reqAllOpenOrders` on startup
Same gap as position reconciliation but for orders. If a sell from a
prior session is still working in IBKR, the engine restart pulls in the
position via reqPositions (after #5 lands) but doesn't see the
outstanding order — it would place a duplicate sell.

**Recommend**: in the position-reconcile path, also call
`reqAllOpenOrders` and populate `entry_orders_` /
`exit_order_symbols_` from the returned orders. Avoids duplicate
sends.

---

## Triage summary

| Priority | Item | Effort |
|---|---|---|
| **Hard blocker** | #1 Contract primaryExchange threading | ~50 LOC + sub-item 9's per-symbol map |
| **Hard blocker** | #5 query_positions / reqPositions wiring | ~100 LOC + 1 unit test |
| **Strong** | #2 reqMarketDataType(1) on connect | ~5 LOC |
| **Strong** | #4 Mutex on send_ts_ / ack_latency_ms_cache_ | ~10 LOC |
| **Strong** | #6 Explicit reqIds(-1) on connect + on error 322 | ~10 LOC |
| **Strong** | #7 Replay subscriptions on reconnect | ~80 LOC + test |
| **Strong** | #8 Error code -> action dispatch | ~60 LOC + test |
| **Observation** | PartiallyFilled handling in engine | ~30 LOC |
| **Observation** | Open-order reconciliation alongside positions | ~50 LOC |
| **Observation** | commissionAndFeesReport wiring | ~30 LOC |

**Total: ~430 LOC across ~10 PRs.** None of it is heavy; this is mostly
plumbing wiring and small mutex fixes.

## Suggested order of work

1. **#4 mutex fix** — pure safety, no behavioral change. Ship first.
2. **#2 reqMarketDataType(1)** — surfaces subscription-missing errors
   early. 5-line change.
3. **#6 reqIds on connect** — prevents subtle order-ID collisions.
4. **#5 query_positions + reqPositions** — unblocks position-reconcile.
5. **#1 primaryExchange + sub-item 9 audit** — landed together since
   they're symbiotic.
6. **#8 error code dispatch** — production hardening.
7. **#7 reconnect subscription replay** — production hardening.
8. **PartiallyFilled + open-order reconcile + commission wiring** —
   robustness when there's an operational issue mid-session.

After items 1-5 land, a short L1-only paper smoke test is safe.
Items 6-8 land alongside the first multi-day endurance run.

## Out-of-scope follow-ups

- The 4 sub-items 6-9 raised here (PartiallyFilled handling, open-order
  reconciliation, commission report wiring, primaryExchange threading)
  could each become their own `#todo` once the user wants to attack
  them. Today they live in this audit document and the umbrella's
  notes section.
- This audit assumes the existing IBKRTransport abstraction is fine.
  An alternative (rewriting against `ib_insync` or `ibapi`'s newer
  asyncio bindings) is out of scope.
