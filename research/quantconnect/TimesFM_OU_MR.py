# TimesFM-OU-MR -- HawkesOU-MR with the Hawkes ranking replaced by
# Google's TimesFM time-series foundation model.
#
# Base skeleton: ChronosOU_MR (same universe, sizing, OU gate, exit,
# reinvest schedule). Only the model swap differs.
#
# Delta from ChronosOU_MR:
#   - Model: google/timesfm-1.0-200m (~200 MB) instead of chronos-t5-tiny.
#   - Batched prediction: TimesFM accepts a LIST of series in one call
#     and returns all forecasts together, so all 49 symbols are scored
#     in a single call per day (Chronos v1 is one-at-a-time).
#   - Point-forecast API rather than sampling-based; we use the point
#     forecast directly as the predicted next-day close.
#
# COMPUTE:
#   TimesFM 200M is ~22x larger than Chronos-tiny (9M). Per-symbol
#   inference is slower, but batching recovers a lot. Rough estimate:
#   49 symbols x 1 batched forecast/day x ~2 s ~ 15-20 min added per
#   year of backtest on CPU (vs ~1 hour/year for Chronos-tiny).
#
# INSTALL:
#   Requires `timesfm` package + `torch`. On QC paid tier, add via
#   Project -> Libraries. Free tier likely won't have it.

from AlgorithmImports import *
import math

try:
    import numpy as np
    import timesfm
    _TIMESFM_AVAILABLE = True
except Exception:
    _TIMESFM_AVAILABLE = False


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

OU_HALFLIFE_SECONDS = 1800.0
OU_BUY_THRESHOLD_PCT = 0.0

TIMESFM_MODEL = "google/timesfm-1.0-200m"
TIMESFM_CONTEXT_LEN = 64
TIMESFM_HORIZON = 1

TRAILING_STOP_PCT = 0.0
BAR_RESOLUTION = Resolution.MINUTE

STRATEGY_NAME = "TimesFM-OU-MR"
START_DATE = (2025, 1, 1)
END_DATE = (2026, 8, 22)
STARTING_CASH = 1700


class SymbolState:
    def __init__(self):
        self.ou_mu = 0.0
        self.ou_initialized = False
        self.last_ou_update_time = None
        self.mid = 0.0
        self.score = 0.0
        self.daily_closes = []
        self.last_daily_close_date = None
        self.entry_price = 0.0
        self.exit_ticket = None


class HftTimesFmOuMR(QCAlgorithm):
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

        self.tfm = None
        if _TIMESFM_AVAILABLE:
            try:
                # v1 API. If your installed timesfm version has a
                # different constructor shape (v2 uses TimesFmHparams
                # / TimesFmCheckpoint dataclasses), adapt here.
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
                self.log(f"TimesFM loaded: {TIMESFM_MODEL}")
            except Exception as e:
                self.log(f"TimesFM load failed: {e}")
        else:
            self.log("timesfm / torch not installed; scores stay at 0")

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

            self._update_ou(st, close, now)
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

        # Batch: collect every symbol with enough history + a valid mid.
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
            # freq=0 means "high-frequency" bucket (daily/higher).
            point_forecast, _ = self.tfm.forecast(
                inputs=batch_inputs,
                freq=[0] * len(batch_inputs),
            )
            # point_forecast shape: [num_series, horizon=1]
            for st, predicted in zip(batch_syms, point_forecast):
                pred_price = float(predicted[0])
                if st.mid > 0.0:
                    st.score = (pred_price - st.mid) / st.mid
        except Exception as e:
            self.log(f"TimesFM forecast batch failed: {e}")

    def _update_ou(self, st, new_mid, now):
        if st.last_ou_update_time is None:
            st.ou_mu = new_mid
            st.ou_initialized = True
            st.last_ou_update_time = now
            return
        dt = (now - st.last_ou_update_time).total_seconds()
        if dt <= 0.0:
            return
        tau = OU_HALFLIFE_SECONDS / math.log(2.0)
        alpha = 1.0 - math.exp(-dt / tau)
        st.ou_mu = (1.0 - alpha) * st.ou_mu + alpha * new_mid
        st.last_ou_update_time = now

    def _current_caps(self):
        realized = max(0.0, float(self.portfolio.total_profit))
        slots_gained = int(realized // BUDGET_INCREMENT)
        top_k = INITIAL_TOP_K + slots_gained
        budget = INITIAL_BUDGET + slots_gained * BUDGET_INCREMENT
        return top_k, budget

    def _route_entries(self):
        top_k, budget = self._current_caps()
        ranked = sorted(self.symbols.items(),
                        key=lambda kv: kv[1].score,
                        reverse=True)
        candidates = ranked[:top_k]

        held = {s for s in self.symbols if self.portfolio[s].invested}
        committed = sum(self.portfolio[s].absolute_holdings_cost for s in held)
        pending_buys = self._pending_buy_symbols()

        for symbol, st in candidates:
            if symbol in held or symbol in pending_buys:
                continue
            if st.mid <= 0.0 or not st.ou_initialized:
                continue
            if st.score <= 0.0:
                continue
            if st.mid > st.ou_mu * (1.0 + OU_BUY_THRESHOLD_PCT):
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
            target = st.entry_price * (1.0 + TARGET_PROFIT_PCT)
            st.exit_ticket = self.limit_order(symbol, -qty, target)
        else:
            st.entry_price = 0.0
            st.exit_ticket = None
