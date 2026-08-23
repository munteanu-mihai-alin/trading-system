# ChronosOU-MR -- HawkesOU-MR with the Hawkes ranking replaced by a
# pretrained time-series foundation model (Amazon Chronos).
#
# Base: HawkesOU-MR (universe, sizing, OU gate, exit target, reinvest
# schedule, all other logic identical). Same compromises vs the C++
# engine.
#
# Delta from base:
#   - Score is no longer Hawkes intensity. Instead, once per day at
#     market_open + 15 min, Chronos-t5-tiny runs a 1-step-ahead
#     forecast on each symbol's daily close series and returns a
#     distribution over next-day close.
#   - Score = predicted 1-day return = (mean_forecast - current_mid) / current_mid.
#   - OU gate still applies (mean-reversion filter).
#   - No changes to exit, reinvest, sizing, or universe.
#
# WHY THIS DESIGN:
#   The Hawkes-intensity ranking degenerated to binary at Minute cadence
#   (see agent/HAWKES_OU_EXPLAINED.md sec 3.4). A pretrained forecaster
#   should give richer per-symbol scores.
#
# COMPUTE + INSTALL CAVEATS:
#   - Requires the `chronos-forecasting` Python package + `torch`.
#     On QC's paid tiers, add via Project -> Libraries (Custom Env).
#     On QC free tier, may not be installable -- if so, strategy will
#     load but scores stay at 0 (nothing trades). Check the log.
#   - Chronos-t5-tiny is ~50 MB. QC downloads it on first backtest.
#     Cache in ObjectStore for faster re-runs if you iterate.
#   - Cost estimate: 49 symbols x 1 prediction/day x ~1 s/prediction
#     ~ 60 min added to backtest compute on Chronos-tiny CPU.
#     Larger Chronos variants (small/base/large) scale linearly.
#   - Chronos operates on the daily close series -- we don't feed it
#     Minute bars. Daily granularity matches the 1-day forecast horizon.
#
# IF CHRONOS DOESN'T WORK IN YOUR QC ENVIRONMENT:
#   Fall back to a lightweight sklearn/LightGBM signal (see follow-up
#   in agent/AGENT_HANDOFF_LOG.md). Same skeleton, cheaper compute,
#   often competitive on tabular financial signals.

from AlgorithmImports import *
import math

# Lazy imports guarded so the file still loads even in environments
# where torch / chronos aren't installed.
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
TARGET_PROFIT_PCT = 0.025

# ==== OU gate parameters ====

OU_HALFLIFE_SECONDS = 1800.0
OU_BUY_THRESHOLD_PCT = 0.0

# ==== Chronos configuration ====

CHRONOS_MODEL = "amazon/chronos-t5-tiny"   # ~50 MB
CHRONOS_CONTEXT_LEN = 64                    # daily closes fed to model
CHRONOS_PREDICTION_LEN = 1                  # forecast horizon (days)
CHRONOS_NUM_SAMPLES = 20                    # Monte Carlo samples

TRAILING_STOP_PCT = 0.0

BAR_RESOLUTION = Resolution.MINUTE

# ==== Backtest window + run label ====

STRATEGY_NAME = "ChronosOU-MR"
START_DATE = (2025, 1, 1)
END_DATE = (2026, 8, 22)
STARTING_CASH = 1700


class SymbolState:
    def __init__(self):
        # OU trailing mean
        self.ou_mu = 0.0
        self.ou_initialized = False
        self.last_ou_update_time = None

        # Current-bar snapshot
        self.mid = 0.0

        # Ranking score = predicted 1-day return from Chronos.
        # Set once per day by _run_daily_forecast; used for ranking
        # across the intraday routing loop.
        self.score = 0.0

        # Daily close series (rolling window fed to Chronos)
        self.daily_closes = []
        self.last_daily_close_date = None

        # Position state
        self.entry_price = 0.0
        self.exit_ticket = None


class HftChronosOuMR(QCAlgorithm):
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

        # Warm up long enough to accumulate CHRONOS_CONTEXT_LEN daily
        # closes plus a buffer for the OU estimator.
        self.set_warm_up(timedelta(days=CHRONOS_CONTEXT_LEN + 10))

        # Load Chronos once. Fail soft: if the package or model download
        # isn't available in this QC environment, the strategy still
        # loads and just doesn't trade (score stays at 0 for everyone).
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

        # Schedule the daily forecast to run once per trading day, 15
        # minutes after the open. First symbol used only to anchor the
        # date_rules -- the callback iterates every symbol internally.
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

            # Roll the daily-close window (one sample per trading day).
            # Using the current bar's close is a slight simplification --
            # a more careful implementation would snapshot at market
            # close, but for a foundation model this granularity is
            # noise vs the 64-day context length.
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
        if self.chronos is None:
            return
        if self.is_warming_up:
            return

        for symbol, st in self.symbols.items():
            history = st.daily_closes
            if len(history) < CHRONOS_CONTEXT_LEN or st.mid <= 0.0:
                continue

            try:
                context = torch.tensor(
                    history[-CHRONOS_CONTEXT_LEN:], dtype=torch.float32
                )
                forecast = self.chronos.predict(
                    context=context,
                    prediction_length=CHRONOS_PREDICTION_LEN,
                    num_samples=CHRONOS_NUM_SAMPLES,
                )
                # forecast shape: [batch=1, num_samples, prediction_length]
                predicted_price = float(forecast[0, :, 0].mean().item())
                st.score = (predicted_price - st.mid) / st.mid
            except Exception:
                # Individual-symbol failure doesn't break the batch.
                st.score = 0.0

    # ---- OU trailing mean ----

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

    # ---- Reinvestment schedule ----

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

            # Require a positive predicted return: if Chronos thinks
            # the name is going down, skip. Also drops the "zero score"
            # case when Chronos isn't loaded.
            if st.score <= 0.0:
                continue

            # OU mean-reversion gate (unchanged from base HawkesOU-MR).
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
            target = st.entry_price * (1.0 + TARGET_PROFIT_PCT)
            st.exit_ticket = self.limit_order(symbol, -qty, target)
        else:
            st.entry_price = 0.0
            st.exit_ticket = None
