# Chronos-MH-Weighted -- Chronos_MR_PredExit variant using a
# multi-horizon weighted forecast.
#
# Base: Chronos_MR_PredExit (no OU gate, sell at model-predicted price
# with never-sell-at-loss fallback, same universe / sizing / reinvest).
#
# Delta from base:
#   - CHRONOS_PREDICTION_LEN raised from 1 to 10. Single Chronos call
#     per symbol per day returns 10 daily forecasts, indexed [0..9].
#   - Three horizons extracted: 1-day, 5-day (~1 trading week),
#     10-day (~2 trading weeks).
#   - Composite score (used for both ranking and entry filter):
#         score = 0.5*ret_1d + 0.3*ret_5d + 0.2*ret_10d
#     where ret_Nd = (predicted_price[N-1] - current_mid) / current_mid.
#     Shorter horizons get more weight (less noisy per step); longer
#     horizons contribute drift signal.
#   - Sell target = 10-day predicted price (with max() fallback to
#     entry * (1 + TARGET_PROFIT_PCT) so we never sell at a loss).
#     Rationale: your holding-time distribution shows p50=4d, p75=10d,
#     so the 10-day predicted price is roughly where positions
#     naturally hit target anyway.
#
# NOTE: extending prediction_length has NO compute penalty per horizon
# step for Chronos-t5 (single autoregressive pass, all horizons in one
# call). The whole thing is still one Chronos call per symbol per day.

from AlgorithmImports import *
import math

try:
    import torch
    from chronos import ChronosPipeline
    _CHRONOS_AVAILABLE = True
except Exception:
    _CHRONOS_AVAILABLE = False


UNIVERSE = [
    "AAPL", "AMAT", "AMD", "AMKR", "APD", "ARM", "ASML", "ASX", "AWK",
    "CDNS", "CEG", "CSCO", "DD", "DELL", "ENTG", "GFS", "GSM", "HPE",
    "HPQ", "HWM", "IBM", "IMOS", "INTC", "KEYS", "KLAC", "LEA", "LIN",
    "LMT", "LRCX", "MKSI", "MU", "NIO", "NOC", "NOK", "NVDA", "OKLO",
    "PSTG", "QCOM", "RTX", "SMCI", "SNPS", "STX", "TSEM", "TSM", "TTE",
    "UMC", "VST", "WDC", "XPEV",
]

INITIAL_TOP_K = 3
TRADE_NOTIONAL = 500.0
INITIAL_BUDGET = 1500.0
BUDGET_INCREMENT = 500.0
TARGET_PROFIT_PCT = 0.025

CHRONOS_MODEL = "amazon/chronos-t5-tiny"
CHRONOS_CONTEXT_LEN = 64
CHRONOS_PREDICTION_LEN = 10               # multi-horizon: covers ~2 trading weeks
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

# Horizon indices into the prediction array (0-based).
HORIZON_1D_IDX = 0
HORIZON_5D_IDX = 4
HORIZON_10D_IDX = 9

# Weights for the composite score. Sum to 1.0. Shorter horizons weight
# more (less noisy per step); longer contributes drift signal.
WEIGHT_1D = 0.5
WEIGHT_5D = 0.3
WEIGHT_10D = 0.2

TRAILING_STOP_PCT = 0.0
BAR_RESOLUTION = Resolution.MINUTE

STRATEGY_NAME = "Chronos-MH-Weighted"
START_DATE = (2025, 1, 1)
END_DATE = (2026, 8, 22)
STARTING_CASH = 1700


class SymbolState:
    def __init__(self):
        # Multi-horizon predicted prices, refreshed daily.
        self.pred_1d = 0.0
        self.pred_5d = 0.0
        self.pred_10d = 0.0

        self.mid = 0.0
        self.daily_closes = []
        self.last_daily_close_date = None
        self.entry_price = 0.0
        self.exit_ticket = None


class HftChronosMhWeighted(QCAlgorithm):
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
                # forecast shape: [batch=1, num_samples, prediction_length=10]
                # Mean across samples at each horizon index.
                st.pred_1d  = float(forecast[0, :, HORIZON_1D_IDX].mean().item())
                st.pred_5d  = float(forecast[0, :, HORIZON_5D_IDX].mean().item())
                st.pred_10d = float(forecast[0, :, HORIZON_10D_IDX].mean().item())
            except Exception:
                pass  # keep stale forecast; single-symbol failure not fatal

        self.last_forecast_time = self.time

    def _forecasts_are_fresh(self):
        if self.last_forecast_time is None:
            return False
        age_hours = (self.time - self.last_forecast_time).total_seconds() / 3600.0
        return age_hours <= FORECAST_MAX_STALENESS_HOURS

    # ---- Multi-horizon composite score ----

    def _return_at(self, predicted_price, mid):
        if predicted_price <= 0.0 or mid <= 0.0:
            return 0.0
        return (predicted_price - mid) / mid

    def _composite_score(self, st):
        r1  = self._return_at(st.pred_1d,  st.mid)
        r5  = self._return_at(st.pred_5d,  st.mid)
        r10 = self._return_at(st.pred_10d, st.mid)
        return WEIGHT_1D * r1 + WEIGHT_5D * r5 + WEIGHT_10D * r10

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

    def _route_entries(self):
        top_k, budget = self._current_caps()

        ranked = sorted(self.symbols.items(),
                        key=lambda kv: self._composite_score(kv[1]),
                        reverse=True)
        candidates = ranked[:top_k]

        held = {s for s in self.symbols if self.portfolio[s].invested}
        committed = sum(self.portfolio[s].absolute_holdings_cost for s in held)
        pending_buys = self._pending_buy_symbols()

        for symbol, st in candidates:
            if symbol in held or symbol in pending_buys:
                continue
            if st.mid <= 0.0 or st.pred_10d <= 0.0:
                continue

            # Entry filter: weighted composite return must clear target.
            if self._composite_score(st) < TARGET_PROFIT_PCT:
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
            # Sell target = 10-day predicted price (matches actual holding
            # distribution better than 1-day). Fallback = entry*(1+tp) so
            # we never place a marketable-loss sell.
            fallback = st.entry_price * (1.0 + TARGET_PROFIT_PCT)
            target = max(st.pred_10d, fallback)
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
