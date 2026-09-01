# TimesFM-MR-PredExit -- TimesFM ranking with no OU gate and
# model-driven sell target.
#
# Base skeleton: Chronos_MR_PredExit (universe, sizing, reinvest,
# predicted-price sell target with never-sell-at-loss fallback).
# Only the model swap differs (Chronos-t5-tiny -> TimesFM 200M).
#
# Delta from Chronos_MR_PredExit:
#   - Model: google/timesfm-1.0-200m instead of chronos-t5-tiny.
#   - Batched prediction (all 49 symbols in one forecast call).
#   - No sampling; point forecast used directly as predicted price.
#
# Compared to TimesFM_OU_MR: same model, but no OU gate + sell target
# from TimesFM's predicted price (not fixed +2.5%).

from AlgorithmImports import *
import math

_TIMESFM_IMPORT_ERROR = None
_TIMESFM_VERSION = None
_TIMESFM_PATH = None
try:
    import numpy as np
    import timesfm
    _TIMESFM_AVAILABLE = True
    _TIMESFM_VERSION = getattr(timesfm, "__version__", "unknown")
    _TIMESFM_PATH = getattr(timesfm, "__file__", "unknown")
except Exception as _e:
    _TIMESFM_AVAILABLE = False
    _TIMESFM_IMPORT_ERROR = f"{type(_e).__name__}: {_e}"


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

TIMESFM_MODEL = "google/timesfm-1.0-200m"
TIMESFM_CONTEXT_LEN = 64
TIMESFM_HORIZON = 1

TRAILING_STOP_PCT = 0.0
BAR_RESOLUTION = Resolution.MINUTE

STRATEGY_NAME = "TimesFM-MR-PredExit"
START_DATE = (2025, 1, 1)
END_DATE = (2026, 8, 22)
STARTING_CASH = 1700


class SymbolState:
    def __init__(self):
        self.predicted_price = 0.0
        self.mid = 0.0
        self.daily_closes = []
        self.last_daily_close_date = None
        self.entry_price = 0.0
        self.exit_ticket = None


class HftTimesFmMrPredExit(QCAlgorithm):
    def initialize(self):
        self.set_start_date(*START_DATE)
        self.set_end_date(*END_DATE)
        start_tag = "{:04d}{:02d}{:02d}".format(*START_DATE)
        end_tag = "{:04d}{:02d}{:02d}".format(*END_DATE)
        self.set_name(
            f"{STRATEGY_NAME}_{start_tag}_{end_tag}"
            f"_target_profit{TARGET_PROFIT_PCT}"
            f"_model{TIMESFM_MODEL.split('/')[-1]}"
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

        warmup_calendar_days = int(TIMESFM_CONTEXT_LEN * 1.6) + 10
        self.set_warm_up(timedelta(days=warmup_calendar_days))

        # ---- Library-availability diagnostics ----
        # Report exactly what's known about the timesfm install so a
        # future run's Logs tab tells us whether the package is
        # missing, at an unexpected version, or in an unexpected path.
        if not _TIMESFM_AVAILABLE:
            self.log(f"timesfm import FAILED: {_TIMESFM_IMPORT_ERROR}")
            self.log("  -> scores stay at 0; strategy will not trade")
            self.log("  -> package likely not in QC's default env (free tier)")
            self.log("  -> paid tier: add via Project -> Libraries (Custom Env)")
        else:
            self.log(f"timesfm import OK: version={_TIMESFM_VERSION}")
            self.log(f"  path={_TIMESFM_PATH}")
            # Log a handful of module attributes so we can see which
            # API surface is installed (v1 uses TimesFm; recent v2 uses
            # TimesFmHparams + TimesFmCheckpoint dataclasses).
            try:
                attrs = [a for a in dir(timesfm) if not a.startswith("_")]
                self.log(f"  attrs (first 20): {attrs[:20]}")
                has_v1 = "TimesFm" in attrs
                has_v2 = "TimesFmHparams" in attrs and "TimesFmCheckpoint" in attrs
                self.log(f"  api-shape: v1-style TimesFm={has_v1} v2-style hparams/ckpt={has_v2}")
            except Exception as attr_err:
                self.log(f"  attr inspection failed: {attr_err}")

        self.tfm = None
        if _TIMESFM_AVAILABLE:
            # ---- Attempt A: v1-style explicit constructor ----
            try:
                self.log("TimesFM: attempting v1-style constructor...")
                self.tfm = timesfm.TimesFm(
                    context_len=512,
                    horizon_len=TIMESFM_HORIZON,
                    input_patch_len=32,
                    output_patch_len=128,
                    num_layers=20,
                    model_dims=1280,
                    backend="cpu",
                )
                self.tfm.load_from_checkpoint(repo_id=TIMESFM_MODEL)
                self.log(f"TimesFM loaded (v1 path): {TIMESFM_MODEL}")
            except Exception as e_v1:
                self.log(f"TimesFM v1-style failed: {type(e_v1).__name__}: {e_v1}")
                self.tfm = None
                # ---- Attempt B: v2-style hparams/checkpoint dataclasses ----
                try:
                    self.log("TimesFM: attempting v2-style hparams constructor...")
                    self.tfm = timesfm.TimesFm(
                        hparams=timesfm.TimesFmHparams(
                            backend="cpu",
                            per_core_batch_size=32,
                            horizon_len=TIMESFM_HORIZON,
                            context_len=512,
                        ),
                        checkpoint=timesfm.TimesFmCheckpoint(
                            huggingface_repo_id=TIMESFM_MODEL,
                        ),
                    )
                    self.log(f"TimesFM loaded (v2 path): {TIMESFM_MODEL}")
                except Exception as e_v2:
                    self.log(f"TimesFM v2-style failed: {type(e_v2).__name__}: {e_v2}")
                    self.log("Both constructor forms failed; scores stay at 0")
                    self.tfm = None

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
                if len(st.daily_closes) > TIMESFM_CONTEXT_LEN * 2:
                    st.daily_closes = st.daily_closes[-TIMESFM_CONTEXT_LEN:]
                st.last_daily_close_date = today

        if self.is_warming_up:
            return

        self._route_entries()

    def _run_daily_forecast(self):
        if self.tfm is None or self.is_warming_up:
            return
        # Skip inference entirely if we can't act on the results.
        if not self._has_free_slot():
            return

        batch_syms = []
        batch_inputs = []
        for symbol, st in self.symbols.items():
            if len(st.daily_closes) < TIMESFM_CONTEXT_LEN or st.mid <= 0.0:
                continue
            batch_syms.append(st)
            batch_inputs.append(
                np.array(st.daily_closes[-TIMESFM_CONTEXT_LEN:], dtype=np.float32)
            )

        if not batch_inputs:
            return

        try:
            point_forecast, _ = self.tfm.forecast(
                inputs=batch_inputs,
                freq=[0] * len(batch_inputs),
            )
            for st, predicted in zip(batch_syms, point_forecast):
                st.predicted_price = float(predicted[0])
        except Exception as e:
            self.log(f"TimesFM forecast batch failed: {e}")

    def _current_return(self, st):
        if st.predicted_price <= 0.0 or st.mid <= 0.0:
            return 0.0
        return (st.predicted_price - st.mid) / st.mid

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
            # Never sell below entry * (1 + target) -- same safety as
            # Chronos_MR_PredExit.
            fallback = st.entry_price * (1.0 + TARGET_PROFIT_PCT)
            target = max(st.predicted_price, fallback)
            st.exit_ticket = self.limit_order(symbol, -qty, target)
        else:
            st.entry_price = 0.0
            st.exit_ticket = None
