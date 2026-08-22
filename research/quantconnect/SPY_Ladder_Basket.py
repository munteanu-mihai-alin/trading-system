# SPY-Ladder-Basket -- SPY-Ladder with a shared exit target across all
# open lots instead of per-lot targets.
#
# Base: SPY_Ladder.py -- same universe (SPLG), same 4-lot / $1000-chunk
# sizing, same scheduled + dip entry logic. Only the exit rule differs.
#
# Delta from base:
#   - Shared-target exit. At any moment, every open lot's limit sell
#     price = max(entry_price across all held lots) * (1 + TARGET_PROFIT_PCT).
#     Example: three lots bought at 280, 275, 260 all sell at 280*1.003
#     = $280.84. The whole basket exits together when SPLG's bid touches
#     the shared target.
#
#   - When a new buy fills at a price HIGHER than any previously-held
#     lot's entry, all existing sell tickets are cancelled and replaced
#     at the new (higher) shared target. When a new buy fills at or
#     below the current max, the shared target is unchanged and only
#     the new lot's sell is placed.
#
#   - When a sell fills, the lot goes free. Remaining held lots keep
#     their existing sell tickets (still at the same shared target).
#     Targets never move down -- only up when a new higher entry
#     raises the max.
#
# Behaviour vs base:
#   - Base sells each lot independently as its own entry * 1.003 fires.
#   - Basket keeps the whole set together until the highest lot's
#     target hits. In an uptrend all lots exit as one big win. In a
#     stagnant/down chop, lower-entry lots that WOULD have hit their
#     own target sit idle waiting for the shared (higher) one.
#
# Not a port of the C++ engine -- pure QC research.

from AlgorithmImports import *


TICKER = "SPLG"
NUM_LOTS = 4
CHUNK_NOTIONAL = 1000.0    # $ per lot; 4 * 1000 = $4000 deployed max
TARGET_PROFIT_PCT = 0.003

DIP_BUY_THRESHOLD_PCT = 0.001

ENTRY_TIMES = [
    (10, 0),
    (11, 30),
    (13, 0),
    (14, 30),
]

BAR_RESOLUTION = Resolution.MINUTE

# ==== Backtest window + run label ====

STRATEGY_NAME = "SPY-Ladder-Basket"
START_DATE = (2026, 1, 1)
END_DATE = (2026, 8, 21)
STARTING_CASH = 4200


class Lot:
    def __init__(self):
        self.qty = 0.0
        self.entry_price = 0.0
        self.buy_ticket = None
        self.sell_ticket = None
        self.has_been_scheduled = False

    def is_free(self):
        return (self.qty == 0.0
                and self.buy_ticket is None
                and self.sell_ticket is None)


class HftSpyLadderBasket(QCAlgorithm):
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

        self.chunk_notional = float(CHUNK_NOTIONAL)
        self.lots = [Lot() for _ in range(NUM_LOTS)]

        self.pending_buys = {}
        self.pending_sells = {}

        self.daily_high = 0.0
        self.current_trading_day = None

        for h, m in ENTRY_TIMES:
            self.schedule.on(
                self.date_rules.every_day(self.symbol),
                self.time_rules.at(h, m),
                self._scheduled_entry_attempt,
            )

    def on_data(self, data: Slice):
        self._sync_ticket_fills()

        if self.symbol not in data.bars:
            return
        price = float(data.bars[self.symbol].close)
        if price <= 0.0:
            return

        today = self.time.date()
        if today != self.current_trading_day:
            self.daily_high = price
            self.current_trading_day = today
        elif price > self.daily_high:
            self.daily_high = price

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
        for lot in self.lots:
            if lot.is_free():
                if self._enter_lot(lot):
                    lot.has_been_scheduled = True
                return

    def _sync_ticket_fills(self):
        for lot in self.lots:
            bt = lot.buy_ticket
            if bt is not None and bt.status == OrderStatus.FILLED:
                self.pending_buys.pop(int(bt.order_id), None)
                lot.entry_price = float(bt.average_fill_price)
                lot.qty = float(bt.quantity_filled)
                lot.buy_ticket = None
                self._handle_buy_fill(lot)

            st = lot.sell_ticket
            if st is not None and st.status == OrderStatus.FILLED:
                self.pending_sells.pop(int(st.order_id), None)
                lot.qty = 0.0
                lot.entry_price = 0.0
                lot.sell_ticket = None

    def _enter_lot(self, lot):
        price = float(self.securities[self.symbol].price)
        if price <= 0.0:
            return False
        qty = int(self.chunk_notional / price)
        if qty <= 0:
            return False
        ticket = self.market_order(self.symbol, qty)
        lot.buy_ticket = ticket
        self.pending_buys[int(ticket.order_id)] = lot
        return True

    # ---- Shared-target exit management ----

    def _handle_buy_fill(self, filled_lot):
        # Compute max entry across OTHER already-held lots (excluding
        # this newly-filled one).
        prev_max = 0.0
        for other in self.lots:
            if other is filled_lot:
                continue
            if other.qty > 0 and other.entry_price > prev_max:
                prev_max = other.entry_price

        if filled_lot.entry_price > prev_max:
            # New lot's entry is the new max -- shared target moves UP.
            # Cancel + replace every held lot's sell at the new target.
            new_target = filled_lot.entry_price * (1.0 + TARGET_PROFIT_PCT)
            self._replace_all_sells_at(new_target)
        else:
            # Shared target unchanged; place ONLY this lot's sell at it.
            shared_target = prev_max * (1.0 + TARGET_PROFIT_PCT)
            self._place_lot_sell(filled_lot, shared_target)

    def _place_lot_sell(self, lot, target):
        ticket = self.limit_order(self.symbol, -lot.qty, target)
        lot.sell_ticket = ticket
        self.pending_sells[int(ticket.order_id)] = lot

    def _replace_all_sells_at(self, target):
        for lot in self.lots:
            if lot.qty <= 0:
                continue
            if lot.sell_ticket is not None:
                self.pending_sells.pop(int(lot.sell_ticket.order_id), None)
                try:
                    lot.sell_ticket.cancel("shared target raised")
                except Exception:
                    pass
                lot.sell_ticket = None
            self._place_lot_sell(lot, target)

    def on_order_event(self, order_event: OrderEvent):
        if order_event.status != OrderStatus.FILLED:
            return
        oid = int(order_event.order_id)

        lot = self.pending_buys.pop(oid, None)
        if lot is not None:
            lot.entry_price = float(order_event.fill_price)
            lot.qty = float(order_event.fill_quantity)
            lot.buy_ticket = None
            self._handle_buy_fill(lot)
            return

        lot = self.pending_sells.pop(oid, None)
        if lot is not None:
            lot.qty = 0.0
            lot.entry_price = 0.0
            lot.sell_ticket = None
