# HawkesOU-MR-Reinvest-Open -- HawkesOU-MR-Reinvest with the mean-
# reversion gate removed.
#
# Base: HawkesOU-MR-Reinvest (reinvest schedule, universe, sizing, exit,
# and all other logic identical). Same compromises vs the C++ engine.
#
# Delta from base:
#   - OU mean-reversion gate DROPPED. No "buy only when mid <= trailing
#     mean" filter. Any top-k Hawkes-ranked candidate is eligible.
#   - OU state is not tracked (no longer needed).
#   - INITIAL_TOP_K stays at 3. The dropped gate is what lets more
#     entries fire; per-bar candidate depth unchanged.
#
# Character change: no longer mean-reversion. This is pure Hawkes
# intensity chasing -- enter the most "active" names, exit at target.
# Expect meaningfully higher trade volume AND meaningfully different
# risk profile: you're now buying names that are moving (in either
# direction), not names that have overshot to the downside.
#
# Reinvest schedule still applies: every $500 realized profit adds
# another slot and another top-k candidate.

from AlgorithmImports import *
import math


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

TRADE_NOTIONAL = 500.0
INITIAL_TOP_K = 3
INITIAL_BUDGET = 1500.0
BUDGET_INCREMENT = 500.0
TARGET_PROFIT_PCT = 0.025

# ==== Signal parameters ====
# NOTE: OU_HALFLIFE_SECONDS / OU_BUY_THRESHOLD_PCT removed -- gate gone.

HAWKES_MU = 10.0
HAWKES_ALPHA = 5.0
HAWKES_BETA = 20.0
HAWKES_MID_CHANGE_THRESHOLD_BPS = 2.5

TRAILING_STOP_PCT = 0.0

BAR_RESOLUTION = Resolution.MINUTE

# ==== Backtest window + run label ====

STRATEGY_NAME = "HawkesOU-MR-Reinvest-Open"
START_DATE = (2025, 1, 1)
END_DATE = (2026, 8, 22)
STARTING_CASH = 1700


class SymbolState:
    def __init__(self):
        # Hawkes only -- no OU state, no mean-reversion gate.
        self.hawkes_lambda = HAWKES_MU
        self.last_hawkes_event_time = None
        self.last_mid_at_event = 0.0

        self.mid = 0.0
        self.score = 0.0

        self.entry_price = 0.0
        self.exit_ticket = None
        self.high_water_bid = 0.0


class HftHawkesOuMrReinvestOpen(QCAlgorithm):
    def initialize(self):
        self.set_start_date(*START_DATE)
        self.set_end_date(*END_DATE)
        start_tag = "{:04d}{:02d}{:02d}".format(*START_DATE)
        end_tag = "{:04d}{:02d}{:02d}".format(*END_DATE)
        self.set_name(
            f"{STRATEGY_NAME}_{start_tag}_{end_tag}_target_profit{TARGET_PROFIT_PCT}"
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

        # Hawkes needs a few observations to elevate above mu -- short
        # warm-up is enough since OU is no longer in the picture.
        self.set_warm_up(timedelta(minutes=15))

    def on_data(self, data: Slice):
        now = self.time

        for symbol, st in self.symbols.items():
            new_mid = self._mid_from_slice(data, symbol)
            if new_mid is None:
                continue

            self._update_hawkes(st, new_mid, now)
            st.mid = new_mid
            st.score = st.hawkes_lambda

        if self.is_warming_up:
            return

        self._route_entries()
        self._update_trailing_stops(data)

    # ---- Reinvestment schedule ----

    def _current_caps(self):
        realized = max(0.0, float(self.portfolio.total_profit))
        slots_gained = int(realized // BUDGET_INCREMENT)
        top_k = INITIAL_TOP_K + slots_gained
        budget = INITIAL_BUDGET + slots_gained * BUDGET_INCREMENT
        return top_k, budget

    # ---- Signal maintenance ----

    def _mid_from_slice(self, data, symbol):
        if symbol in data.quote_bars:
            qb = data.quote_bars[symbol]
            if qb.bid is not None and qb.ask is not None:
                return 0.5 * (qb.bid.close + qb.ask.close)
        if symbol in data.bars:
            return data.bars[symbol].close
        return None

    def _update_hawkes(self, st, new_mid, now):
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
            if st.mid <= 0.0:
                continue

            # No mean-reversion gate -- any candidate qualifies.

            if committed + TRADE_NOTIONAL > budget:
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
            st.exit_ticket = self.limit_order(symbol, -qty, target)
        else:
            st.entry_price = 0.0
            st.high_water_bid = 0.0
            st.exit_ticket = None
