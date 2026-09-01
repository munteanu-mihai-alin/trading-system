# TimesFM-Fin-MR-PredExit -- variant of TimesFM_MR_PredExit that uses
# Preferred Networks' financial fine-tune of TimesFM (`pfnet/timesfm-
# 1.0-200m-fin`) instead of Google's vanilla checkpoint.
#
# Base: TimesFM_MR_PredExit (universe, sizing, reinvest, no OU gate,
# sell at model-predicted price with never-sell-at-loss fallback).
#
# WHY THIS EXISTS:
#   Google's `google/timesfm-1.0-200m` was pretrained on generic time
#   series (energy, weather, retail, etc.). PFN fine-tuned the same
#   200M-param architecture on TOPIX500 + S&P500 daily/hourly stock
#   series + crypto + FX through 2022-12-31. Their reported Sharpe
#   on long-horizon S&P500 daily strategies:
#     - Base google/timesfm-1.0-200m : 0.42
#     - pfnet/timesfm-1.0-200m-fin   : 1.68
#   (Source: arXiv 2412.09880 -- Preferred Networks tech blog.)
#
# TWO CRITICAL DIFFERENCES vs the vanilla TimesFM_MR_PredExit file:
#
#   1. PREPROCESSING. The fine-tune expects inputs in log-space and
#      returns outputs in log-space:
#           x_in = log(x + 1)
#           forecast_log = model(x_in)
#           forecast     = exp(forecast_log) - 1
#      Skipping this makes the model useless (predictions in the wrong
#      numeric scale). Vanilla TimesFM doesn't need this.
#
#   2. CHECKPOINT LOAD PATH. Known unresolved bug (HF discussion #1,
#      open Apr 2025+): the HF repo ships pytorch_model.bin +
#      model.safetensors, but timesfm's `load_from_checkpoint(repo_id=)`
#      expects `torch_model.ckpt`. We snapshot_download the repo
#      manually, then attempt load. If it fails, the log tells you
#      exactly what to try (usually a rename or format conversion).
#
# ENVIRONMENT REQUIREMENTS (same as vanilla TimesFM):
#   - `timesfm` Python package (NOT in QC's default library set --
#     paid tier + Custom Environment required).
#   - `torch`, `jax`, `huggingface_hub`.
#   - Python 3.10 (hard constraint from timesfm pkg).
#   On QC free tier this will fail-soft (scores stay at 0, strategy
#   loads but never trades) -- same failure mode as our earlier
#   TimesFM run.
#
# LICENSE: Apache-2.0 (both timesfm package and the pfnet weights).

from AlgorithmImports import *
import math

try:
    import numpy as np
    import timesfm
    from huggingface_hub import snapshot_download
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

TIMESFM_MODEL = "pfnet/timesfm-1.0-200m-fin"
TIMESFM_CONTEXT_LEN = 64                        # trading closes used per prediction
TIMESFM_MODEL_CONTEXT = 512                     # architecture's max context (left-pad if shorter)
TIMESFM_HORIZON = 1                             # 1-day-ahead forecast

TRAILING_STOP_PCT = 0.0
BAR_RESOLUTION = Resolution.MINUTE

STRATEGY_NAME = "TimesFM-Pfnet-Fin-MR-PredExit"
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


class HftTimesFmPfnetFinMrPredExit(QCAlgorithm):
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
                # Build the model architecture (same 200M-param spec as
                # google/timesfm-1.0-200m).
                self.tfm = timesfm.TimesFm(
                    context_len=TIMESFM_MODEL_CONTEXT,
                    horizon_len=128,     # architecture default; we index first
                    input_patch_len=32,
                    output_patch_len=128,
                    num_layers=20,
                    model_dims=1280,
                    backend="cpu",
                )
                # Try direct repo_id load first (works if HF checkpoint
                # bug ever gets fixed upstream).
                try:
                    self.tfm.load_from_checkpoint(repo_id=TIMESFM_MODEL)
                    self.log(f"TimesFM-Fin loaded (repo_id path): {TIMESFM_MODEL}")
                except Exception as direct_err:
                    self.log(f"repo_id load failed ({direct_err}); trying snapshot_download workaround...")
                    local_dir = snapshot_download(repo_id=TIMESFM_MODEL)
                    # Attempt load from local path -- timesfm may still
                    # complain about the expected filename; if so the
                    # error surfaces and can be triaged from the log.
                    self.tfm.load_from_checkpoint(checkpoint_path=local_dir)
                    self.log(f"TimesFM-Fin loaded (snapshot path): {local_dir}")
            except Exception as e:
                self.log(f"TimesFM-Fin load failed: {e}")
                self.tfm = None
        else:
            self.log("timesfm / huggingface_hub not installed; scores stay at 0")

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

        # Batch: one forecast call for all eligible symbols.
        batch_syms = []
        batch_inputs = []
        for symbol, st in self.symbols.items():
            if len(st.daily_closes) < TIMESFM_CONTEXT_LEN or st.mid <= 0.0:
                continue
            batch_syms.append(st)
            # Log-space preprocessing (required by the pfnet fine-tune).
            # Take last TIMESFM_CONTEXT_LEN closes, apply log(x+1).
            raw = np.array(
                st.daily_closes[-TIMESFM_CONTEXT_LEN:], dtype=np.float32
            )
            log_series = np.log(raw + 1.0)
            batch_inputs.append(log_series)

        if not batch_inputs:
            return

        try:
            # TimesFM 1.0 batched forecast API.
            point_forecast, _quantile_forecast = self.tfm.forecast(
                inputs=batch_inputs,
                freq=[0] * len(batch_inputs),
            )
            # point_forecast shape: [num_series, horizon_len=128]
            for st, log_pred in zip(batch_syms, point_forecast):
                # Take just the first horizon step and invert the
                # log preprocessing: price = exp(log_pred) - 1.
                log_next = float(log_pred[0])
                st.predicted_price = float(np.exp(log_next) - 1.0)
        except Exception as e:
            self.log(f"TimesFM-Fin forecast batch failed: {e}")

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
            fallback = st.entry_price * (1.0 + TARGET_PROFIT_PCT)
            target = max(st.predicted_price, fallback)
            st.exit_ticket = self.limit_order(symbol, -qty, target)
        else:
            st.entry_price = 0.0
            st.exit_ticket = None
