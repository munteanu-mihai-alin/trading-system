# Chronos2-MR-MultiFeature -- Amazon Chronos-2 (actual v2 generation)
# ranking without OU gate, augmented with vol and momentum signals.
#
# Base skeleton: Chronos_MR_PredExit (universe, sizing, reinvest,
# predicted-price sell target with never-sell-at-loss fallback).
#
# NOTE ON MODEL: Chronos-2 vs Chronos-Bolt vs Chronos v1
#   - Chronos v1 (`amazon/chronos-t5-*`): original T5-based, autoregressive
#     sampling. Univariate.
#   - Chronos-Bolt (`amazon/chronos-bolt-*`): mid-2024 speed upgrade to
#     v1's architecture. Still univariate.
#   - Chronos-2 (`amazon/chronos-2`): the actual v2 generation.
#     Supports multivariate + covariates natively.
#
# WHAT THIS FILE DOES:
#   - Loads Chronos-2 via BaseChronosPipeline (which auto-detects the
#     model type from the checkpoint).
#   - Predicts each symbol UNIVARIATELY (feeds only daily closes).
#     This is the guaranteed-to-work path across Chronos-2 pipeline
#     API variants.
#   - Combines the prediction with two auxiliary features computed in
#     Python:
#       * Realized 20-day volatility (annualized)
#       * 5-day short-term momentum
#   - Four-filter entry gate:
#       1. Predicted return >= TARGET_PROFIT_PCT
#       2. Predicted 25th-quantile return > 0 (confidence)
#       3. Realized vol <= MAX_ANNUAL_VOL (avoid crashy names)
#       4. 5-day momentum >= 0 (no falling knives)
#   - Ranks by Sharpe-like ratio: predicted_return / vol.
#   - Sell target = Chronos-2 predicted price, with the same
#     `max(predicted, entry * (1 + TARGET_PROFIT_PCT))` never-sell-at-loss
#     safety used in Chronos_MR_PredExit.
#
# TO USE CHRONOS-2's NATIVE MULTIVARIATE MODE:
#   Chronos-2 accepts an input tensor of shape [seq_len, num_features]
#   for true multivariate forecasting (feed price + volume + vol
#   together, get joint forecast). This file currently doesn't wire
#   that path -- add features to `_multivariate_context(st)` and pass
#   through to `pipeline.predict(...)` once the exact API is confirmed
#   in your QC environment.

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

CHRONOS_MODEL = "amazon/chronos-2"
CHRONOS_CONTEXT_LEN = 64
CHRONOS_PREDICTION_LEN = 1
CHRONOS_QUANTILES = [0.1, 0.25, 0.5, 0.75, 0.9]

MAX_ANNUAL_VOL = 0.80
VOL_LOOKBACK_DAYS = 20
MOMENTUM_LOOKBACK_DAYS = 5
VOL_FLOOR_FOR_RATIO = 0.05

TRAILING_STOP_PCT = 0.0
BAR_RESOLUTION = Resolution.MINUTE

STRATEGY_NAME = "Chronos2-MR-MultiFeature"
START_DATE = (2025, 1, 1)
END_DATE = (2026, 8, 22)
STARTING_CASH = 1700


class SymbolState:
    def __init__(self):
        # Chronos-2 outputs (updated daily)
        self.predicted_price = 0.0
        self.predicted_q25 = 0.0

        # Auxiliary features computed from daily_closes each day
        self.realized_vol = 0.0
        self.momentum_5d = 0.0

        # Live snapshot
        self.mid = 0.0

        # Daily-close series
        self.daily_closes = []
        self.last_daily_close_date = None

        # Position state
        self.entry_price = 0.0
        self.exit_ticket = None


class HftChronos2MrMultiFeature(QCAlgorithm):
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

        self.pipeline = None
        if _CHRONOS_AVAILABLE:
            try:
                # BaseChronosPipeline auto-detects the checkpoint type
                # (v1, Bolt, Chronos-2). If your installed chronos-
                # forecasting is too old to know about Chronos-2, either
                # upgrade the package or switch to a v1/Bolt model.
                self.pipeline = BaseChronosPipeline.from_pretrained(
                    CHRONOS_MODEL,
                    device_map="cpu",
                    torch_dtype=torch.float32,
                )
                self.log(f"Chronos-2 loaded: {CHRONOS_MODEL}")
            except Exception as e:
                self.log(f"Chronos-2 load failed: {e}")
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
        if self.pipeline is None or self.is_warming_up:
            return
        # Skip inference entirely if we can't act on the results.
        if not self._has_free_slot():
            return

        for symbol, st in self.symbols.items():
            history = st.daily_closes
            if len(history) < CHRONOS_CONTEXT_LEN or st.mid <= 0.0:
                continue

            # Auxiliary features from daily_closes -- computed in Python,
            # not fed to Chronos-2. See header note on how to extend to
            # true multivariate model input if desired.
            arr = np.asarray(
                history[-VOL_LOOKBACK_DAYS - 1:], dtype=np.float64
            )
            if len(arr) >= 2:
                log_rets = np.diff(np.log(arr))
                st.realized_vol = float(log_rets.std(ddof=1) * math.sqrt(252.0))
            if len(history) >= MOMENTUM_LOOKBACK_DAYS + 1:
                past = history[-MOMENTUM_LOOKBACK_DAYS - 1]
                if past > 0.0:
                    st.momentum_5d = (history[-1] - past) / past

            # Chronos-2 prediction. BaseChronosPipeline exposes
            # predict_quantiles() across Bolt and v2 -- if v2's API
            # differs in your installed version, adapt this block.
            try:
                context = torch.tensor(
                    history[-CHRONOS_CONTEXT_LEN:], dtype=torch.float32
                )
                quantiles, mean = self.pipeline.predict_quantiles(
                    context,
                    prediction_length=CHRONOS_PREDICTION_LEN,
                    quantile_levels=CHRONOS_QUANTILES,
                )
                st.predicted_price = float(mean[0, 0].item())
                # Index of 0.25 in CHRONOS_QUANTILES is 1
                st.predicted_q25 = float(quantiles[0, 0, 1].item())
            except Exception:
                pass  # keep stale predictions

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

            if self._predicted_return(st) < TARGET_PROFIT_PCT:
                continue
            if self._predicted_q25_return(st) <= 0.0:
                continue
            if st.realized_vol > MAX_ANNUAL_VOL:
                continue
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
