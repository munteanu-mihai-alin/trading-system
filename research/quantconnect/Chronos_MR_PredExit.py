# Chronos-MR-PredExit -- Chronos-only variant with model-driven sell target.
#
# Base skeleton: ChronosOU_MR (universe, sizing, reinvest schedule).
#
# Delta from base:
#   - OU mean-reversion gate DROPPED. Entries are driven purely by
#     Chronos's predicted 1-day return.
#   - Entry filter: only buy when current predicted return
#     (predicted_price / current_mid - 1) is >= TARGET_PROFIT_PCT.
#     "Only enter if the model thinks the upside is at least what we
#     want to make."
#   - Sell target = Chronos's PREDICTED PRICE (not entry * (1+target)).
#     If Chronos predicted next-day close = $155 and we entered at
#     $151, we sell at $155 -- whatever the model forecast.
#   - Ranking: by CURRENT predicted return (recomputed each bar from
#     the stored predicted_price and the live mid). So as intraday
#     price moves close the gap, the effective ranking updates without
#     needing a fresh Chronos call.
#
# Design intent: this file is a pure "trust the model" variant. If
# Chronos's edge is real, this should outperform ChronosOU_MR because
# the OU gate isn't filtering out legitimate rallies. If it underperforms,
# you've learned OU was doing more of the entry work than Chronos.
#
# Same COMPUTE + INSTALL caveats as ChronosOU_MR (see that file's header).

from AlgorithmImports import *
import math

try:
    import torch
    from chronos import ChronosPipeline
    _CHRONOS_AVAILABLE = True
except Exception:
    _CHRONOS_AVAILABLE = False


# ==== Universe (mirrors config.databento_backtest.yen.ini) ====

UNIVERSE = [
    "AAPL", "AMAT", "AMD", "AMKR", "APD", "ARM", "ASML", "ASX", "AWK",
    "CDNS", "CEG", "CSCO", "DD", "DELL", "ENTG", "GFS", "GSM", "HPE",
    "HPQ", "HWM", "IBM", "IMOS", "INTC", "KEYS", "KLAC", "LEA", "LIN",
    "LMT", "LRCX", "MKSI", "MU", "NIO", "NOC", "NOK", "NVDA", "OKLO",
    "PSTG", "QCOM", "RTX", "SMCI", "SNPS", "STX", "TSEM", "TSM", "TTE",
    "UMC", "VST", "WDC", "XPEV",
]

# ==== Sizing & reinvest schedule ====

INITIAL_TOP_K = 3
TRADE_NOTIONAL = 500.0
INITIAL_BUDGET = 1500.0
BUDGET_INCREMENT = 500.0
TARGET_PROFIT_PCT = 0.025      # entry filter + fallback sell target

# ==== Chronos configuration ====

CHRONOS_MODEL = "amazon/chronos-t5-tiny"
CHRONOS_CONTEXT_LEN = 64
CHRONOS_PREDICTION_LEN = 1
CHRONOS_NUM_SAMPLES = 20

# ==== Forecast-refresh policy ====
# See Chronos_MH_Consensus.py header for full doc. Three modes:
#   "always_daily"  -- always fire scheduled 10:15 AM trigger.
#   "fresh_gate"    -- skip only if slots full AND predictions <
#                      FORECAST_MAX_STALENESS_HOURS old.
#   "on_slot_free"  -- skip scheduled trigger if slots full; force
#                      immediate refresh on sell fill.
FORECAST_REFRESH_POLICY = "on_slot_free"
FORECAST_MAX_STALENESS_HOURS = 24.0

TRAILING_STOP_PCT = 0.0
BAR_RESOLUTION = Resolution.MINUTE

# ==== Backtest window + run label ====

STRATEGY_NAME = "Chronos-MR-PredExit"
START_DATE = (2025, 1, 1)
END_DATE = (2026, 8, 22)
STARTING_CASH = 1700


class SymbolState:
    def __init__(self):
        # Signal state (updated once per day by Chronos forecast).
        # predicted_price: mean of Chronos sample paths for next day.
        # score cached only for legacy reasons -- not consulted for
        # ranking; ranking uses the freshly-computed current return.
        self.predicted_price = 0.0

        # Live snapshot -- updated every bar in on_data.
        self.mid = 0.0

        # Daily-close series fed to Chronos.
        self.daily_closes = []
        self.last_daily_close_date = None

        # Position state.
        self.entry_price = 0.0
        self.exit_ticket = None


class HftChronosMrPredExit(QCAlgorithm):
    def initialize(self):
        self.set_start_date(*START_DATE)
        self.set_end_date(*END_DATE)
        start_tag = "{:04d}{:02d}{:02d}".format(*START_DATE)
        end_tag = "{:04d}{:02d}{:02d}".format(*END_DATE)
        self.set_name(
            f"{STRATEGY_NAME}_{start_tag}_{end_tag}"
            f"_target_profit{TARGET_PROFIT_PCT}"
            f"_model{CHRONOS_MODEL.split('/')[-1]}"
        )

        self.set_cash(STARTING_CASH)
        self.set_time_zone(TimeZones.NEW_YORK)

        self.symbols = {}
        for tk in UNIVERSE:
            eq = self.add_equity(
                tk, BAR_RESOLUTION,
                data_normalization_mode=DataNormalizationMode.RAW,
            )
            self.symbols[eq.symbol] = SymbolState()

        # Warm-up long enough to accumulate CHRONOS_CONTEXT_LEN TRADING
        # closes.
        warmup_calendar_days = int(CHRONOS_CONTEXT_LEN * 1.6) + 10
        self.set_warm_up(timedelta(days=warmup_calendar_days))

        # Stamped by _forecast_all_symbols; consulted by the
        # "fresh_gate" refresh policy.
        self.last_forecast_time = None

        self.chronos = None
        if _CHRONOS_AVAILABLE:
            try:
                self.chronos = ChronosPipeline.from_pretrained(
                    CHRONOS_MODEL,
                    device_map="cpu",
                    torch_dtype=torch.float32,
                )
                self.log(f"Chronos loaded: {CHRONOS_MODEL}")
            except Exception as e:
                self.log(f"Chronos load failed: {e}")
        else:
            self.log("chronos-forecasting / torch not installed; scores stay at 0")

        first_symbol = next(iter(self.symbols.keys()))
        self.schedule.on(
            self.date_rules.every_day(first_symbol),
            self.time_rules.after_market_open(first_symbol, 15),
            self._run_daily_forecast,
        )

    def on_data(self, data: Slice):
        now = self.time
        today = now.date()

        for symbol, st in self.symbols.items():
            if symbol not in data.bars:
                continue
            close = float(data.bars[symbol].close)
            if close <= 0.0:
                continue

            st.mid = close

            if st.last_daily_close_date != today:
                st.daily_closes.append(close)
                if len(st.daily_closes) > CHRONOS_CONTEXT_LEN * 2:
                    st.daily_closes = st.daily_closes[-CHRONOS_CONTEXT_LEN:]
                st.last_daily_close_date = today

        if self.is_warming_up:
            return

        self._route_entries()

    # ---- Chronos forecasting ----

    def _run_daily_forecast(self):
        # Scheduled entry point. Policy-based decision on whether to
        # invoke Chronos now. See FORECAST_REFRESH_POLICY docstring.
        if self.chronos is None or self.is_warming_up:
            return

        policy = FORECAST_REFRESH_POLICY
        if policy == "always_daily":
            pass
        elif policy == "fresh_gate":
            if not self._has_free_slot() and self._forecasts_are_fresh():
                return
        elif policy == "on_slot_free":
            if not self._has_free_slot():
                return

        self._forecast_all_symbols()

    def _forecast_all_symbols(self):
        # Actual inference loop. Called from _run_daily_forecast and
        # from on_order_event sell-fill under the "on_slot_free" policy.
        for symbol, st in self.symbols.items():
            history = st.daily_closes
            if len(history) < CHRONOS_CONTEXT_LEN or st.mid <= 0.0:
                continue

            try:
                context = torch.tensor(
                    history[-CHRONOS_CONTEXT_LEN:], dtype=torch.float32
                )
                forecast = self.chronos.predict(
                    context,
                    prediction_length=CHRONOS_PREDICTION_LEN,
                    num_samples=CHRONOS_NUM_SAMPLES,
                )
                # [batch=1, num_samples, prediction_length=1]
                st.predicted_price = float(forecast[0, :, 0].mean().item())
            except Exception:
                pass

        self.last_forecast_time = self.time

    def _forecasts_are_fresh(self):
        if self.last_forecast_time is None:
            return False
        age_hours = (self.time - self.last_forecast_time).total_seconds() / 3600.0
        return age_hours <= FORECAST_MAX_STALENESS_HOURS

    # ---- Predicted-return helper ----

    def _current_return(self, st):
        # Recomputed each call so intraday mid moves update the
        # effective score without needing another Chronos call.
        if st.predicted_price <= 0.0 or st.mid <= 0.0:
            return 0.0
        return (st.predicted_price - st.mid) / st.mid

    # ---- Reinvestment schedule ----

    def _has_free_slot(self):
        # Cheap capacity check used to short-circuit expensive daily
        # inference when we have no room to enter another position.
        # Mirrors the budget test in _route_entries.
        _, budget = self._current_caps()
        held = {s for s in self.symbols if self.portfolio[s].invested}
        committed = sum(self.portfolio[s].absolute_holdings_cost for s in held)
        return committed + TRADE_NOTIONAL <= budget

    def _current_caps(self):
        realized = max(0.0, float(self.portfolio.total_profit))
        slots_gained = int(realized // BUDGET_INCREMENT)
        top_k = INITIAL_TOP_K + slots_gained
        budget = INITIAL_BUDGET + slots_gained * BUDGET_INCREMENT
        return top_k, budget

    # ---- Order routing ----

    def _route_entries(self):
        top_k, budget = self._current_caps()

        ranked = sorted(self.symbols.items(),
                        key=lambda kv: self._current_return(kv[1]),
                        reverse=True)
        candidates = ranked[:top_k]

        held = {s for s in self.symbols if self.portfolio[s].invested}
        committed = sum(self.portfolio[s].absolute_holdings_cost for s in held)
        pending_buys = self._pending_buy_symbols()

        for symbol, st in candidates:
            if symbol in held or symbol in pending_buys:
                continue
            if st.mid <= 0.0 or st.predicted_price <= 0.0:
                continue

            # Entry filter: only buy if the model's predicted upside
            # from HERE is at least our target profit.
            current_ret = self._current_return(st)
            if current_ret < TARGET_PROFIT_PCT:
                continue

            if committed + TRADE_NOTIONAL > budget:
                break

            qty = int(TRADE_NOTIONAL / st.mid)
            if qty <= 0:
                continue

            self.market_order(symbol, qty)
            committed += qty * st.mid

    def _pending_buy_symbols(self):
        pending = set()
        for order in self.transactions.get_open_orders():
            if order.direction == OrderDirection.BUY:
                pending.add(order.symbol)
        return pending

    # ---- Fill handling ----

    def on_order_event(self, order_event: OrderEvent):
        if order_event.status != OrderStatus.FILLED:
            return

        symbol = order_event.symbol
        if symbol not in self.symbols:
            return
        st = self.symbols[symbol]

        if order_event.direction == OrderDirection.BUY:
            st.entry_price = float(order_event.fill_price)
            qty = float(order_event.fill_quantity)

            # Sell target = Chronos's predicted price at the last daily
            # forecast. If slippage on entry pushed us above the predicted
            # price (would create an instant-loss limit), fall back to a
            # safe target of entry * (1 + TARGET_PROFIT_PCT) so we never
            # place a marketable sell below entry.
            fallback = st.entry_price * (1.0 + TARGET_PROFIT_PCT)
            target = max(st.predicted_price, fallback)

            st.exit_ticket = self.limit_order(symbol, -qty, target)
        else:
            st.entry_price = 0.0
            st.exit_ticket = None
            # A slot just freed. Under "on_slot_free" the scheduled
            # 10:15 AM trigger may not fire again for hours -- force a
            # fresh forecast now so the next _route_entries call ranks
            # on current, not stale, predictions.
            if FORECAST_REFRESH_POLICY == "on_slot_free" \
                    and self.chronos is not None \
                    and not self.is_warming_up:
                self._forecast_all_symbols()
