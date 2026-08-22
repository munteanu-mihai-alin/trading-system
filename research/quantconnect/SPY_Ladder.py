# SPY-Ladder -- $1700 staggered across 6 S&P 500 ETF lots, each with
# its own +TARGET_PROFIT_PCT target sell. Steady-state re-entries wait
# for a dip from the day's high; scheduled events cap idle time.
#
# Not a port of the C++ engine -- pure QC research.
#
# Design:
#   - $1700 / 6 = ~$283 per lot. Six independent lots on a single
#     symbol, each with its own entry price and +TARGET_PROFIT_PCT
#     limit sell target.
#
#   - Initial ramp: at each of the 6 scheduled times (10:00, 11:00,
#     ..., 15:00 NY), the callback enters ONE free lot. Day 1 sees 6
#     sequential entries staggered across the session -- 6 different
#     cost bases.
#
#   - Steady state, dip trigger: after a lot has been scheduled at
#     least once (has_been_scheduled = True), on_data enters it as
#     soon as SPLG's current price is DIP_BUY_THRESHOLD_PCT below the
#     day's high-water. Reference resets at each new trading day.
#     Rationale: don't buy back a fresh lot at the same price we just
#     sold at.
#
#   - Steady state, scheduled fallback: the 6 scheduled events also
#     enter ANY free lot they find. If no dip occurs, capital idles at
#     worst until the next scheduled hour.
#
#   - Per-lot target: each buy fill immediately places a
#     +TARGET_PROFIT_PCT limit sell for the lot's exact quantity.
#     Never sells at a loss -- an underwater lot sits open until the
#     target eventually fills.
#
# Ticker choice: SPLG rather than SPY. SPLG is the SPDR Portfolio S&P
# 500 ETF -- same S&P 500 exposure as SPY, but trades around $70/share
# vs SPY's $500+. At $283 per chunk, SPLG gives us ~3-4 whole shares
# per lot. Plain SPY would round down to 0 shares and never trade.
# Change TICKER to "SPY" if you enable fractional shares in QC or if
# STARTING_CASH grows enough that $283 buys a whole SPY share.

from AlgorithmImports import *


TICKER = "SPLG"
NUM_LOTS = 6
TARGET_PROFIT_PCT = 0.005

# Dip re-entry: after a lot has been used at least once, on_data will
# re-enter it once the current price is this fraction below the day's
# high-water. Set 0.0 to re-enter every bar regardless of price.
DIP_BUY_THRESHOLD_PCT = 0.004

# Initial-ramp + fallback entry times, one lot per event. NY hours;
# avoids the first 30 minutes (opening auction volatility) and the
# last hour (close auction risk).
ENTRY_TIMES = [
    (10, 0),
    (11, 0),
    (12, 0),
    (13, 0),
    (14, 0),
    (15, 0),
]

BAR_RESOLUTION = Resolution.MINUTE

# ==== Backtest window + run label ====

STRATEGY_NAME = "SPY-Ladder"
START_DATE = (2026, 1, 1)
END_DATE = (2026, 8, 21)
STARTING_CASH = 1700


class Lot:
    def __init__(self):
        self.qty = 0.0
        self.entry_price = 0.0
        self.buy_ticket = None
        self.sell_ticket = None
        # True once this lot has been through its first entry. Blocks
        # the dip trigger from firing before the initial ramp finishes.
        self.has_been_scheduled = False

    def is_free(self):
        return (self.qty == 0.0
                and self.buy_ticket is None
                and self.sell_ticket is None)


class HftSpyLadder(QCAlgorithm):
    def initialize(self):
        self.set_start_date(*START_DATE)
        self.set_end_date(*END_DATE)
        start_tag = "{:04d}{:02d}{:02d}".format(*START_DATE)
        end_tag = "{:04d}{:02d}{:02d}".format(*END_DATE)
        self.set_name(
            f"{STRATEGY_NAME}_{start_tag}_{end_tag}"
            f"_target_profit{TARGET_PROFIT_PCT}"
            f"_dip{DIP_BUY_THRESHOLD_PCT}"
        )

        self.set_cash(STARTING_CASH)
        self.set_time_zone(TimeZones.NEW_YORK)

        equity = self.add_equity(
            TICKER, BAR_RESOLUTION,
            data_normalization_mode=DataNormalizationMode.RAW,
        )
        self.symbol = equity.symbol

        self.chunk_notional = float(STARTING_CASH) / NUM_LOTS
        self.lots = [Lot() for _ in range(NUM_LOTS)]

        # Order-id -> lot maps. Populated at market_order / limit_order
        # return time (captured as int) so on_order_event can look up
        # the originating lot by integer key -- more robust than
        # comparing OrderTicket.order_id against OrderEvent.order_id,
        # which can silently mismatch on QC's cloud runner.
        self.pending_buys = {}
        self.pending_sells = {}

        # Day-scoped dip reference.
        self.daily_high = 0.0
        self.current_trading_day = None

        # Scheduled events: each fires once per trading day at its
        # slot time and enters one free lot (regardless of which).
        for h, m in ENTRY_TIMES:
            self.schedule.on(
                self.date_rules.every_day(self.symbol),
                self.time_rules.at(h, m),
                self._scheduled_entry_attempt,
            )

    def on_data(self, data: Slice):
        if self.symbol not in data.bars:
            return
        price = float(data.bars[self.symbol].close)
        if price <= 0.0:
            return

        # Roll the daily high-water reference.
        today = self.time.date()
        if today != self.current_trading_day:
            self.daily_high = price
            self.current_trading_day = today
        elif price > self.daily_high:
            self.daily_high = price

        # Dip trigger: current price is DIP_BUY_THRESHOLD_PCT below
        # today's high. Enter one free lot per bar (avoid stacking).
        if self.daily_high <= 0.0:
            return
        dip_price = self.daily_high * (1.0 - DIP_BUY_THRESHOLD_PCT)
        if price > dip_price:
            return

        for lot in self.lots:
            if lot.is_free() and lot.has_been_scheduled:
                self._enter_lot(lot)
                return

    def _scheduled_entry_attempt(self):
        # Each scheduled event enters one free lot (max one per event).
        # Serves as both the initial ramp (day 1 fills 6 lots one per
        # hour) and the steady-state fallback if no dip ever fires.
        for lot in self.lots:
            if lot.is_free():
                if self._enter_lot(lot):
                    lot.has_been_scheduled = True
                return

    def _enter_lot(self, lot):
        price = float(self.securities[self.symbol].price)
        if price <= 0.0:
            return False
        qty = int(self.chunk_notional / price)
        if qty <= 0:
            # Notional too small for a whole share at current price.
            # User can grow chunk_notional or switch TICKER.
            return False
        ticket = self.market_order(self.symbol, qty)
        lot.buy_ticket = ticket
        self.pending_buys[int(ticket.order_id)] = lot
        return True

    def on_order_event(self, order_event: OrderEvent):
        if order_event.status != OrderStatus.FILLED:
            return
        oid = int(order_event.order_id)

        lot = self.pending_buys.pop(oid, None)
        if lot is not None:
            lot.entry_price = float(order_event.fill_price)
            lot.qty = float(order_event.fill_quantity)
            lot.buy_ticket = None
            target = lot.entry_price * (1.0 + TARGET_PROFIT_PCT)
            sell_ticket = self.limit_order(self.symbol, -lot.qty, target)
            lot.sell_ticket = sell_ticket
            self.pending_sells[int(sell_ticket.order_id)] = lot
            return

        lot = self.pending_sells.pop(oid, None)
        if lot is not None:
            # Target sell filled -- lot is free. Next entry comes from
            # either a dip trigger (on_data) or the next scheduled event.
            lot.qty = 0.0
            lot.entry_price = 0.0
            lot.sell_ticket = None
