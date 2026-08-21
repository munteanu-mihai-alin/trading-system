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
#      default here is Minute. Flip BAR_RESOLUTION to Resolution.Second
#      for a closer analogue (much more expensive to backtest).
#   3. Hawkes event trigger. C++ uses "mid moved >= 2.5 bps since last
#      firing" as the event proxy (DatabentoBacktestBroker doesn't emit
#      trades). Same proxy applied here to bar-close mids. Intensity is
#      NOT decayed on read -- matches C++, which only updates on event.
#   4. Entry price. C++ places a marketable limit at the L1 ask
#      (`entry_limit_mode=ask`). QC MarketOrder gives us marketable-at-fill;
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

BAR_RESOLUTION = Resolution.Minute


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


class HftMeanReversion(QCAlgorithm):
    def Initialize(self):
        # Yen-unwind adversarial window from the source config.
        self.SetStartDate(2024, 8, 2)
        self.SetEndDate(2024, 8, 9)
        self.SetCash(5000)
        self.SetTimeZone(TimeZones.NewYork)

        self.symbols = {}
        for tk in UNIVERSE:
            eq = self.AddEquity(tk, BAR_RESOLUTION,
                                dataNormalizationMode=DataNormalizationMode.Raw)
            self.symbols[eq.Symbol] = SymbolState()

        # Give OU a full half-life to prime before any trading.
        self.SetWarmUp(timedelta(minutes=60))

    def OnData(self, data: Slice):
        now = self.Time

        for symbol, st in self.symbols.items():
            new_mid = self._mid_from_slice(data, symbol)
            if new_mid is None:
                continue

            self._update_hawkes(st, new_mid, now)
            self._update_ou(st, new_mid, now)
            st.mid = new_mid
            st.score = st.hawkes_lambda

        if self.IsWarmingUp:
            return

        self._route_entries()
        self._update_trailing_stops(data)

    # ---- Signal maintenance ----

    def _mid_from_slice(self, data, symbol):
        # Prefer L1 mid from QuoteBar; fall back to trade close if quotes
        # aren't in this slice.
        if data.QuoteBars.ContainsKey(symbol):
            qb = data.QuoteBars[symbol]
            if qb.Bid is not None and qb.Ask is not None:
                return 0.5 * (qb.Bid.Close + qb.Ask.Close)
        if data.Bars.ContainsKey(symbol):
            return data.Bars[symbol].Close
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

        held = {s for s, _ in self.symbols.items() if self.Portfolio[s].Invested}
        committed = sum(self.Portfolio[s].AbsoluteHoldingsCost for s in held)
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

            self.MarketOrder(symbol, qty)
            committed += qty * st.mid

    def _update_trailing_stops(self, data):
        if TRAILING_STOP_PCT <= 0.0:
            return
        for symbol, st in self.symbols.items():
            if not self.Portfolio[symbol].Invested or st.entry_price <= 0.0:
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
                st.exit_ticket.Cancel("trailing stop retrace -- crossing to market")
            self.MarketOrder(symbol, -self.Portfolio[symbol].Quantity)

    def _bid_from_slice(self, data, symbol):
        if data.QuoteBars.ContainsKey(symbol):
            qb = data.QuoteBars[symbol]
            if qb.Bid is not None:
                return qb.Bid.Close
        if data.Bars.ContainsKey(symbol):
            return data.Bars[symbol].Close
        return None

    def _pending_buy_symbols(self):
        pending = set()
        for order in self.Transactions.GetOpenOrders():
            if order.Direction == OrderDirection.Buy:
                pending.add(order.Symbol)
        return pending

    # ---- Fill handling ----

    def OnOrderEvent(self, order_event: OrderEvent):
        if order_event.Status != OrderStatus.Filled:
            return

        symbol = order_event.Symbol
        if symbol not in self.symbols:
            return
        st = self.symbols[symbol]

        if order_event.Direction == OrderDirection.Buy:
            st.entry_price = float(order_event.FillPrice)
            st.high_water_bid = st.entry_price
            qty = float(order_event.FillQuantity)
            target = st.entry_price * (1.0 + TARGET_PROFIT_PCT)
            # Passive limit at target. QC crosses this as soon as bid >= target,
            # matching C++'s clamped sell-limit semantics.
            st.exit_ticket = self.LimitOrder(symbol, -qty, target)
        else:
            st.entry_price = 0.0
            st.high_water_bid = 0.0
            st.exit_ticket = None
