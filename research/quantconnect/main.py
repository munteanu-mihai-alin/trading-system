# QuantConnect port of the HFT mean-reversion strategy.
#
# Source of truth: this repo's C++ engine, backtest configuration
# `config.databento_backtest.yen.ini`. Compromises are called out in
# COMPROMISES below.
#
# COMPROMISES (relative to the C++ engine on Databento MBP-10 L2):
#   1. No L2 depth on QC's standard equity feed. The C++ backtest already
#      turns off the synthetic FillModel (`synthetic_fill_model=false`),
#      so score reduces to Hawkes intensity -- portable as-is. What we
#      lose is the L2-derived microprice, bid/ask depth-imbalance, and
#      queue-ahead numbers the sell-side execution score uses. Here we
#      just fire the exit as a passive limit at the target -- same net
#      effect since C++ clamps the sell limit up to the current bid.
#   2. Ranking cadence. C++ ranks every L1 top-of-book update (~64x/sec
#      per symbol on MBP-1). QC's cheapest useful cadence is Minute; the
#      default here is Minute. Flip BAR_RESOLUTION to Resolution.SECOND
#      for a closer analogue (much more expensive to backtest).
#   3. Hawkes event trigger. C++ uses "mid moved >= 2.5 bps since last
#      firing" as the event proxy (DatabentoBacktestBroker doesn't emit
#      trades). Same proxy applied here to bar-close mids. Intensity is
#      NOT decayed on read -- matches C++, which only updates on event.
#   4. Entry price. C++ places a marketable limit at the L1 ask
#      (`entry_limit_mode=ask`). QC market_order gives us marketable-at-fill;
#      slippage model absorbs the microstructure difference.
#   5. Costs. QC applies its own equity fee + slippage model. C++ has an
#      explicit commission/half-spread/impact block; magnitudes are
#      comparable (IB-tier commissions ~$0.005/share). Compare the two
#      cost models when calibrating.
#   6. Trailing stop OFF (matches yen config: `trailing_stop_pct` unset).
#   7. Universe frozen to the 49 yen-window symbols; SNDK excluded per
#      the source config. All other symbol_universe_path drift not tracked.

from AlgorithmImports import *
import math


# ==== Configuration (mirrors config.databento_backtest.yen.ini) ====

UNIVERSE = [
    "AAPL", "AMAT", "AMD", "AMKR", "APD", "ARM", "ASML", "ASX", "AWK",
    "CDNS", "CEG", "CSCO", "DD", "DELL", "ENTG", "GFS", "GSM", "HPE",
    "HPQ", "HWM", "IBM", "IMOS", "INTC", "KEYS", "KLAC", "LEA", "LIN",
    "LMT", "LRCX", "MKSI", "MU", "NIO", "NOC", "NOK", "NVDA", "OKLO",
    "PSTG", "QCOM", "RTX", "SMCI", "SNPS", "STX", "TSEM", "TSM", "TTE",
    "UMC", "VST", "WDC", "XPEV",
]

TOP_K = 3
TRADE_NOTIONAL = 500.0
ACCOUNT_BUDGET = 1500.0
TARGET_PROFIT_PCT = 0.008

# Mean-reversion gate: buy only when mid <= ou_mu * (1 + threshold).
# threshold=0.0 means "at or below the trailing OU mean".
OU_HALFLIFE_SECONDS = 1800.0
OU_BUY_THRESHOLD_PCT = 0.0

# Hawkes: lambda = mu + (lambda - mu) * exp(-beta * dt) + alpha * event
HAWKES_MU = 10.0
HAWKES_ALPHA = 5.0
HAWKES_BETA = 20.0
HAWKES_MID_CHANGE_THRESHOLD_BPS = 2.5

# Trailing stop off by default (matches yen config).
TRAILING_STOP_PCT = 0.0

BAR_RESOLUTION = Resolution.MINUTE

# Backtest window + display name. Keep as constants so set_name() in
# initialize() always encodes the period into the run name -- any time
# you sweep the window, the run label updates automatically.
STRATEGY_NAME = "HawkesOU-MR"
START_DATE = (2026, 1, 1)
END_DATE = (2026, 8, 21)


class SymbolState:
    def __init__(self):
        # Hawkes intensity, updated only on threshold-cross events.
        self.hawkes_lambda = HAWKES_MU
        self.last_hawkes_event_time = None
        self.last_mid_at_event = 0.0

        # OU trailing mean via dt-weighted EWMA (half-life in wall-clock).
        self.ou_mu = 0.0
        self.ou_initialized = False
        self.last_ou_update_time = None

        # Current-bar snapshot.
        self.mid = 0.0
        self.score = 0.0

        # Position state.
        self.entry_price = 0.0
        self.exit_ticket = None
        self.high_water_bid = 0.0


class HftHawkesOuMR(QCAlgorithm):
    def initialize(self):
        self.set_start_date(*START_DATE)
        self.set_end_date(*END_DATE)
        # e.g. HawkesOU-MR_20260101_20260821 -- visible in the QC UI so
        # sweeps across different windows stay distinguishable at a glance.
        start_tag = "{:04d}{:02d}{:02d}".format(*START_DATE)
        end_tag = "{:04d}{:02d}{:02d}".format(*END_DATE)
        self.set_name(f"{STRATEGY_NAME}_{start_tag}_{end_tag}")

        self.set_cash(5000)
        self.set_time_zone(TimeZones.NEW_YORK)

        self.symbols = {}
        for tk in UNIVERSE:
            eq = self.add_equity(
                tk, BAR_RESOLUTION,
                data_normalization_mode=DataNormalizationMode.RAW,
            )
            self.symbols[eq.symbol] = SymbolState()

        # Give OU a full half-life to prime before any trading.
        self.set_warm_up(timedelta(minutes=60))

    def on_data(self, data: Slice):
        now = self.time

        for symbol, st in self.symbols.items():
            new_mid = self._mid_from_slice(data, symbol)
            if new_mid is None:
                continue

            self._update_hawkes(st, new_mid, now)
            self._update_ou(st, new_mid, now)
            st.mid = new_mid
            st.score = st.hawkes_lambda

        if self.is_warming_up:
            return

        self._route_entries()
        self._update_trailing_stops(data)

    # ---- Signal maintenance ----

    def _mid_from_slice(self, data, symbol):
        # Prefer L1 mid from QuoteBar; fall back to trade close if quotes
        # aren't in this slice.
        if symbol in data.quote_bars:
            qb = data.quote_bars[symbol]
            if qb.bid is not None and qb.ask is not None:
                return 0.5 * (qb.bid.close + qb.ask.close)
        if symbol in data.bars:
            return data.bars[symbol].close
        return None

    def _update_hawkes(self, st, new_mid, now):
        # First observation: prime and return.
        if st.last_hawkes_event_time is None:
            st.last_mid_at_event = new_mid
            st.last_hawkes_event_time = now
            return

        if st.last_mid_at_event <= 0.0:
            st.last_mid_at_event = new_mid
            return

        move_bps = abs(new_mid - st.last_mid_at_event) / st.last_mid_at_event * 1e4
        if move_bps < HAWKES_MID_CHANGE_THRESHOLD_BPS:
            return

        dt = max((now - st.last_hawkes_event_time).total_seconds(), 1e-6)
        decay = math.exp(-HAWKES_BETA * dt)
        st.hawkes_lambda = HAWKES_MU + (st.hawkes_lambda - HAWKES_MU) * decay + HAWKES_ALPHA
        st.last_hawkes_event_time = now
        st.last_mid_at_event = new_mid

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

    # ---- Order routing ----

    def _route_entries(self):
        ranked = sorted(self.symbols.items(),
                        key=lambda kv: kv[1].score,
                        reverse=True)
        top_k = ranked[:TOP_K]

        held = {s for s in self.symbols if self.portfolio[s].invested}
        committed = sum(self.portfolio[s].absolute_holdings_cost for s in held)
        pending_buys = self._pending_buy_symbols()

        for symbol, st in top_k:
            if symbol in held or symbol in pending_buys:
                continue
            if st.mid <= 0.0 or not st.ou_initialized:
                continue

            # Mean-reversion gate: only buy at-or-below the trailing OU mean.
            if st.mid > st.ou_mu * (1.0 + OU_BUY_THRESHOLD_PCT):
                continue

            if committed + TRADE_NOTIONAL > ACCOUNT_BUDGET:
                break

            qty = int(TRADE_NOTIONAL / st.mid)
            if qty <= 0:
                continue

            self.market_order(symbol, qty)
            committed += qty * st.mid

    def _update_trailing_stops(self, data):
        if TRAILING_STOP_PCT <= 0.0:
            return
        for symbol, st in self.symbols.items():
            if not self.portfolio[symbol].invested or st.entry_price <= 0.0:
                continue
            bid = self._bid_from_slice(data, symbol)
            if bid is None:
                continue
            if bid > st.high_water_bid:
                st.high_water_bid = bid
            target = st.entry_price * (1.0 + TARGET_PROFIT_PCT)
            if bid <= target:
                continue
            floor = st.high_water_bid * (1.0 - TRAILING_STOP_PCT)
            if bid > floor:
                continue
            if st.exit_ticket is not None:
                st.exit_ticket.cancel("trailing stop retrace -- crossing to market")
            self.market_order(symbol, -self.portfolio[symbol].quantity)

    def _bid_from_slice(self, data, symbol):
        if symbol in data.quote_bars:
            qb = data.quote_bars[symbol]
            if qb.bid is not None:
                return qb.bid.close
        if symbol in data.bars:
            return data.bars[symbol].close
        return None

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
            st.high_water_bid = st.entry_price
            qty = float(order_event.fill_quantity)
            target = st.entry_price * (1.0 + TARGET_PROFIT_PCT)
            # Passive limit at target. QC crosses this as soon as bid >= target,
            # matching C++'s clamped sell-limit semantics.
            st.exit_ticket = self.limit_order(symbol, -qty, target)
        else:
            st.entry_price = 0.0
            st.high_water_bid = 0.0
            st.exit_ticket = None
