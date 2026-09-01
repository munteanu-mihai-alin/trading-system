# ChronosBolt-MR-MultiSignal -- Chronos v2 (Bolt) ranking without OU
# gate, augmented with volatility and momentum signals at the strategy
# level.
#
# Base skeleton: Chronos_MR_PredExit (universe, sizing, reinvest,
# predicted-price sell target with never-sell-at-loss fallback).
#
# Delta from Chronos_MR_PredExit:
#   - Model: amazon/chronos-bolt-tiny (Chronos v2 architecture, ~9M
#     params). Uses BaseChronosPipeline.predict_quantiles() which
#     returns both quantile levels and mean directly -- no Monte Carlo
#     sampling loop, dramatically faster than Chronos v1.
#   - Adds THREE additional signals combined at the strategy level:
#       1. Chronos-Bolt 25th-quantile: filter for prediction confidence
#          (even the pessimistic case must be positive).
#       2. Realized volatility (20-day rolling std of daily log
#          returns, annualized): filter and reweight.
#       3. Short-term momentum (5-day return): filter to avoid
#          "falling knife" entries.
#   - Ranking score = predicted_mean_return / max(vol, 0.05).
#     Sharpe-like ratio -- prefers high predicted return AT LOW
#     volatility instead of raw expected return.
#
# NOTE ON "MULTIVARIATE":
#   Chronos-Bolt (like all Chronos variants) is a UNIVARIATE model at
#   the architecture level -- it only accepts one series per prediction.
#   The "multi-feature" nature of this strategy comes from computing
#   vol / momentum separately in Python and combining them with
#   Chronos's price prediction in the ranking and filter logic. For
#   TRUE multivariate at the model level (multiple input series per
#   symbol), Salesforce's Moirai supports it -- would be a separate
#   strategy file.
#
# INSTALL:
#   Requires the same chronos-forecasting package as ChronosOU_MR;
#   Bolt models are part of that package. Package availability is the
#   dominant install concern on QC free tier.

from AlgorithmImports import *
import math

try:
    import numpy as np
    import torch
    from chronos import BaseChronosPipeline
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

# Chronos-Bolt configuration
CHRONOS_MODEL = "amazon/chronos-bolt-tiny"     # v2 architecture
CHRONOS_CONTEXT_LEN = 64
CHRONOS_PREDICTION_LEN = 1
CHRONOS_QUANTILES = [0.1, 0.25, 0.5, 0.75, 0.9]

# ==== Forecast-refresh policy ====
# See Chronos_MH_Consensus.py header for full doc. Three modes:
#   "always_daily"  -- always fire scheduled 10:15 AM trigger.
#   "fresh_gate"    -- skip only if slots full AND predictions <
#                      FORECAST_MAX_STALENESS_HOURS old.
#   "on_slot_free"  -- skip scheduled trigger if slots full; force
#                      immediate refresh on sell fill.
FORECAST_REFRESH_POLICY = "on_slot_free"
FORECAST_MAX_STALENESS_HOURS = 24.0

# Multi-signal thresholds
MAX_ANNUAL_VOL = 0.80          # skip names with > 80% annualized vol
VOL_LOOKBACK_DAYS = 20         # window for realized vol computation
MOMENTUM_LOOKBACK_DAYS = 5     # window for short-term return check
VOL_FLOOR_FOR_RATIO = 0.05     # denominator floor -- avoid /0 for stable names

TRAILING_STOP_PCT = 0.0
BAR_RESOLUTION = Resolution.MINUTE

STRATEGY_NAME = "ChronosBolt-MR-MultiSignal"
START_DATE = (2025, 1, 1)
END_DATE = (2026, 8, 22)
STARTING_CASH = 1700


class SymbolState:
    def __init__(self):
        # Chronos-Bolt outputs (updated daily)
        self.predicted_price = 0.0
        self.predicted_q25 = 0.0

        # Auxiliary features computed from daily_closes each day
        self.realized_vol = 0.0     # annualized
        self.momentum_5d = 0.0      # 5-day return

        # Live snapshot
        self.mid = 0.0

        # Daily-close series
        self.daily_closes = []
        self.last_daily_close_date = None

        # Position state
        self.entry_price = 0.0
        self.exit_ticket = None


class HftChronosBoltMrMultiSignal(QCAlgorithm):
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

        self.pipeline = None
        if _CHRONOS_AVAILABLE:
            try:
                self.pipeline = BaseChronosPipeline.from_pretrained(
                    CHRONOS_MODEL,
                    device_map="cpu",
                    torch_dtype=torch.float32,
                )
                self.log(f"Chronos-Bolt loaded: {CHRONOS_MODEL}")
            except Exception as e:
                self.log(f"Chronos-Bolt load failed: {e}")
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

    # ---- Daily forecast + auxiliary features ----

    def _run_daily_forecast(self):
        # Scheduled entry point. Policy-based decision on whether to
        # invoke Chronos now. See FORECAST_REFRESH_POLICY docstring.
        if self.pipeline is None or self.is_warming_up:
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

            # Auxiliary features from the same daily-close series --
            # cheap to compute, no ML needed.
            arr = np.asarray(history[-VOL_LOOKBACK_DAYS - 1:], dtype=np.float64)
            if len(arr) >= 2:
                log_rets = np.diff(np.log(arr))
                st.realized_vol = float(log_rets.std(ddof=1) * math.sqrt(252.0))
            if len(history) >= MOMENTUM_LOOKBACK_DAYS + 1:
                past = history[-MOMENTUM_LOOKBACK_DAYS - 1]
                if past > 0.0:
                    st.momentum_5d = (history[-1] - past) / past

            # Chronos-Bolt prediction (quantiles + mean).
            try:
                context = torch.tensor(
                    history[-CHRONOS_CONTEXT_LEN:], dtype=torch.float32
                )
                quantiles, mean = self.pipeline.predict_quantiles(
                    context,
                    prediction_length=CHRONOS_PREDICTION_LEN,
                    quantile_levels=CHRONOS_QUANTILES,
                )
                # mean shape:      [batch=1, prediction_length=1]
                # quantiles shape: [batch=1, prediction_length=1, num_quantiles]
                st.predicted_price = float(mean[0, 0].item())
                # Index of 0.25 in CHRONOS_QUANTILES is 1
                st.predicted_q25 = float(quantiles[0, 0, 1].item())
            except Exception:
                pass  # keep stale prediction

        self.last_forecast_time = self.time

    def _forecasts_are_fresh(self):
        if self.last_forecast_time is None:
            return False
        age_hours = (self.time - self.last_forecast_time).total_seconds() / 3600.0
        return age_hours <= FORECAST_MAX_STALENESS_HOURS

    # ---- Composite scoring ----

    def _predicted_return(self, st):
        if st.predicted_price <= 0.0 or st.mid <= 0.0:
            return 0.0
        return (st.predicted_price - st.mid) / st.mid

    def _predicted_q25_return(self, st):
        if st.predicted_q25 <= 0.0 or st.mid <= 0.0:
            return 0.0
        return (st.predicted_q25 - st.mid) / st.mid

    def _composite_score(self, st):
        # Sharpe-like ranking: expected return per unit of realized vol.
        pr = self._predicted_return(st)
        return pr / max(st.realized_vol, VOL_FLOOR_FOR_RATIO)

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
                        key=lambda kv: self._composite_score(kv[1]),
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

            # Filter 1: predicted expected return must meet target.
            if self._predicted_return(st) < TARGET_PROFIT_PCT:
                continue
            # Filter 2: model confidence -- 25th quantile must be
            # positive (even the pessimistic case is upside).
            if self._predicted_q25_return(st) <= 0.0:
                continue
            # Filter 3: vol not excessive.
            if st.realized_vol > MAX_ANNUAL_VOL:
                continue
            # Filter 4: not a falling knife (5-day momentum >= 0).
            if st.momentum_5d < 0.0:
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
                    and self.pipeline is not None \
                    and not self.is_warming_up:
                self._forecast_all_symbols()
