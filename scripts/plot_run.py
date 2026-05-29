#!/usr/bin/env python3
"""Lightweight backtest postmortem plotter + metrics.

Reads the per-run folder produced by `hft_app` and renders:
  - plots/equity_curve.png       cumulative PnL with buy/sell markers
  - plots/<SYMBOL>.png           per traded symbol: price + entry/exit + target
  - plots/pnl_per_trade.png      bar chart, colored by win/loss
  - metrics.json                 the computed risk/return summary
  - metrics.md                   human-readable rendering of the same

Usage:
  python scripts/plot_run.py reports/runs/<run_id>
  python scripts/plot_run.py reports/runs/<run_id> --l1-dir data/l1

Skipped silently if a CSV is missing; emits a single warning line.
"""

from __future__ import annotations

import argparse
import configparser
import json
import math
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional

import matplotlib

matplotlib.use("Agg")
import matplotlib.dates as mdates
import matplotlib.pyplot as plt
import pandas as pd

# Annualisation: ~390 RTH minute-bars per day, ~252 trading days per year.
MINUTES_PER_YEAR = 390 * 252


# ---------------------------- IO helpers ---------------------------- #


def read_csv_skipping_comments(path: Path) -> pd.DataFrame:
    """Reads a CSV that may have `# session_*` comment lines from the engine.

    pandas' `comment='#'` handles both the session_start/end markers and any
    other future `#`-prefixed annotations.
    """
    if not path.exists():
        return pd.DataFrame()
    return pd.read_csv(path, comment="#")


def read_config(run_dir: Path) -> Dict[str, str]:
    cfg = configparser.ConfigParser()
    cfg_path = run_dir / "config.ini"
    if not cfg_path.exists():
        return {}
    cfg.read(cfg_path)
    out: Dict[str, str] = {}
    for section in cfg.sections():
        for key, value in cfg.items(section):
            out[key] = value
    return out


# ----------------------- Round-trip derivation ---------------------- #


@dataclass
class RoundTrip:
    symbol: str
    buy_ts_ns: int  # wall-clock when the engine wrote the row
    sell_ts_ns: Optional[int]
    buy_step: int  # engine step index at buy fill
    sell_step: Optional[int]
    qty: float
    entry_price: float
    exit_price: Optional[float]
    open_at_end: bool
    # Simulated market timestamps derived from L1's ts_event[step]. Filled in
    # later by `attach_market_timestamps` once the L1 frame for the symbol is
    # available. Plotting uses these so markers line up with the price line.
    buy_market_ts_ns: Optional[int] = None
    sell_market_ts_ns: Optional[int] = None

    @property
    def pnl_gross(self) -> float:
        if self.exit_price is None:
            return 0.0
        return (self.exit_price - self.entry_price) * self.qty

    @property
    def holding_minutes(self) -> float:
        """Real-time held duration. Engine timestamps are wall-clock, which
        compresses the simulated window — useful as "engine seconds spent"
        but not "market minutes between buy and sell".
        """
        if self.sell_ts_ns is None:
            return float("nan")
        return (self.sell_ts_ns - self.buy_ts_ns) / 60_000_000_000.0

    @property
    def holding_market_minutes(self) -> float:
        """Simulated market-time held duration in minutes. None if we couldn't
        attach a market timestamp (legacy CSV without ts_event)."""
        if self.buy_market_ts_ns is None or self.sell_market_ts_ns is None:
            return float("nan")
        return (
            self.sell_market_ts_ns - self.buy_market_ts_ns
        ) / 60_000_000_000.0


def derive_round_trips(
    orders: pd.DataFrame, commission_per_share: float, min_per_order: float
) -> List[RoundTrip]:
    """Pairs each filled buy with the next filled sell on the same symbol.

    Open positions (no sell fill yet) are returned with open_at_end=True.
    """
    if orders.empty:
        return []
    filled = orders[orders["event"] == "filled"].copy()
    filled.sort_values("ts_ns", inplace=True)

    open_by_symbol: Dict[str, Dict[str, Any]] = {}
    trips: List[RoundTrip] = []
    for _, row in filled.iterrows():
        symbol = row["symbol"]
        side = row["side"]
        if side == "buy":
            open_by_symbol[symbol] = {
                "buy_ts_ns": int(row["ts_ns"]),
                "buy_step": int(row["step"]),
                "qty": float(row["filled_qty"]),
                "entry": float(row["avg_fill_price"]),
            }
        elif side == "sell" and symbol in open_by_symbol:
            entry = open_by_symbol.pop(symbol)
            trips.append(
                RoundTrip(
                    symbol=symbol,
                    buy_ts_ns=entry["buy_ts_ns"],
                    sell_ts_ns=int(row["ts_ns"]),
                    buy_step=entry["buy_step"],
                    sell_step=int(row["step"]),
                    qty=entry["qty"],
                    entry_price=entry["entry"],
                    exit_price=float(row["avg_fill_price"]),
                    open_at_end=False,
                )
            )

    # Any leftovers are still-open positions at end of run.
    for symbol, state in open_by_symbol.items():
        trips.append(
            RoundTrip(
                symbol=symbol,
                buy_ts_ns=state["buy_ts_ns"],
                sell_ts_ns=None,
                buy_step=state["buy_step"],
                sell_step=None,
                qty=state["qty"],
                entry_price=state["entry"],
                exit_price=None,
                open_at_end=True,
            )
        )
    return trips


def attach_market_timestamps(trips: List[RoundTrip],
                              l1_by_symbol: Dict[str, pd.DataFrame]) -> None:
    """Maps engine step -> L1.ts_event[step] per symbol so markers can land
    on the simulated market wall-clock rather than the engine's execution
    wall-clock. Mutates `trips` in place; leaves market timestamps as None
    when the L1 frame is missing or doesn't carry ts_event."""
    for t in trips:
        l1 = l1_by_symbol.get(t.symbol)
        if l1 is None or l1.empty or "ts_event" not in l1.columns:
            continue
        # L1 step column equals row index; clamp at end if step >= size.
        max_idx = len(l1) - 1
        buy_idx = min(int(t.buy_step), max_idx)
        t.buy_market_ts_ns = int(l1["ts_event"].iat[buy_idx])
        if t.sell_step is not None:
            sell_idx = min(int(t.sell_step), max_idx)
            t.sell_market_ts_ns = int(l1["ts_event"].iat[sell_idx])


def apply_commissions(
    trips: List[RoundTrip], per_share: float, min_per_order: float
) -> List[float]:
    """Returns per-trip net PnL after both-leg commissions.

    Open positions get only the buy commission; the sell is hypothetical.
    """
    net: List[float] = []
    for t in trips:
        buy_comm = max(per_share * t.qty, min_per_order)
        if t.open_at_end:
            net.append(t.pnl_gross - buy_comm)
        else:
            sell_comm = max(per_share * t.qty, min_per_order)
            net.append(t.pnl_gross - buy_comm - sell_comm)
    return net


# ----------------------- Equity curve + metrics --------------------- #


def build_equity_curve(
    trips: List[RoundTrip], net_pnl: List[float]
) -> pd.DataFrame:
    """Equity curve sampled at each sell event timestamp (simulated market
    time when available, engine wall-clock as fallback).

    Open positions don't contribute to realized equity here. The curve has
    one point per closed round-trip and a starting zero point.
    """
    rows = [{"ts_ns": 0, "pnl_cum": 0.0, "symbol": "_start", "leg": "init"}]
    cum = 0.0
    closed = [(t, pnl) for t, pnl in zip(trips, net_pnl) if not t.open_at_end]
    closed.sort(
        key=lambda p: p[0].sell_market_ts_ns
        if p[0].sell_market_ts_ns is not None
        else p[0].sell_ts_ns
    )
    for t, pnl in closed:
        cum += pnl
        ts = (
            t.sell_market_ts_ns
            if t.sell_market_ts_ns is not None
            else t.sell_ts_ns
        )
        rows.append({"ts_ns": ts, "pnl_cum": cum, "symbol": t.symbol,
                     "leg": "exit"})
    df = pd.DataFrame(rows)
    # The synthetic zero starting point is at epoch 0 by default; that yanks
    # the plot x-axis back to 1970. Anchor it to a hair before the first real
    # event so the curve starts where the action does.
    if len(df) > 1:
        df.loc[0, "ts_ns"] = int(df.loc[1, "ts_ns"]) - 60_000_000_000
    df["ts"] = pd.to_datetime(df["ts_ns"], unit="ns", utc=True)
    return df


def _l1_mid(l1_df: pd.DataFrame) -> pd.Series:
    """Returns the mid series indexed by ts_event (ns int64), or empty."""
    if l1_df is None or l1_df.empty:
        return pd.Series(dtype=float)
    mid = 0.5 * (l1_df["bid_price"] + l1_df["ask_price"])
    return pd.Series(mid.values, index=l1_df["ts_event"].astype("int64").values)


def compute_holding_analytics(
    trips: List[RoundTrip],
    l1_by_symbol: Dict[str, pd.DataFrame],
) -> List[Dict[str, Any]]:
    """For each trip (open or closed): how deep underwater did it go and what
    fraction of holding time was the price below entry. These are the only
    losses the engine can produce given the current always-win sell logic."""
    out: List[Dict[str, Any]] = []
    for t in trips:
        l1 = l1_by_symbol.get(t.symbol)
        if l1 is None or l1.empty or t.buy_market_ts_ns is None:
            out.append({
                "symbol": t.symbol, "max_drawdown_pct": None,
                "pct_time_below_entry": None, "days_held_market": None,
                "open_at_end": t.open_at_end,
            })
            continue
        mid = _l1_mid(l1)
        # Holding window: buy ts -> sell ts (closed) OR buy ts -> last L1 ts (open)
        start_ns = t.buy_market_ts_ns
        end_ns = (t.sell_market_ts_ns if t.sell_market_ts_ns is not None
                  else int(l1["ts_event"].max()))
        window_mid = mid[(mid.index >= start_ns) & (mid.index <= end_ns)]
        if window_mid.empty:
            out.append({
                "symbol": t.symbol, "max_drawdown_pct": None,
                "pct_time_below_entry": None, "days_held_market": None,
                "open_at_end": t.open_at_end,
            })
            continue
        min_mid = float(window_mid.min())
        max_dd_pct = (min_mid - t.entry_price) / t.entry_price * 100.0
        pct_under = float((window_mid < t.entry_price).mean()) * 100.0
        days_held = (end_ns - start_ns) / 86_400_000_000_000.0  # ns -> days
        out.append({
            "symbol": t.symbol,
            "max_drawdown_pct": round(max_dd_pct, 4),
            "pct_time_below_entry": round(pct_under, 2),
            "days_held_market": round(days_held, 2),
            "open_at_end": t.open_at_end,
        })
    return out


def compute_unrealized_pnl(
    trips: List[RoundTrip],
    l1_by_symbol: Dict[str, pd.DataFrame],
) -> float:
    """Mark-to-market unrealized PnL on positions open at end of run."""
    total = 0.0
    for t in trips:
        if not t.open_at_end:
            continue
        l1 = l1_by_symbol.get(t.symbol)
        if l1 is None or l1.empty:
            continue
        last_mid = 0.5 * (l1["bid_price"].iat[-1] + l1["ask_price"].iat[-1])
        total += (last_mid - t.entry_price) * t.qty
    return total


def compute_time_weighted_equity(
    trips: List[RoundTrip],
    net_pnl: List[float],
    l1_by_symbol: Dict[str, pd.DataFrame],
) -> pd.DataFrame:
    """Builds a 1-minute-bar equity curve over the union of all held-symbol
    L1 timestamps. Each row: ts (market wall-clock), realized PnL up to and
    including any trades closed by ts, sum of mark-to-market unrealized PnL
    over positions held at ts. The total column (realized + unrealized) is
    what Sortino / Calmar / max-drawdown are computed against. Returns an
    empty frame when no L1 data is available."""
    held_symbols = sorted({t.symbol for t in trips})
    if not held_symbols:
        return pd.DataFrame()
    # Build a master ts index from all held symbols' L1.
    ts_pieces = []
    for sym in held_symbols:
        l1 = l1_by_symbol.get(sym)
        if l1 is None or l1.empty or "ts_event" not in l1.columns:
            continue
        ts_pieces.append(l1["ts_event"].astype("int64"))
    if not ts_pieces:
        return pd.DataFrame()
    master_ts = pd.Index(sorted(set(pd.concat(ts_pieces, ignore_index=True))))
    if master_ts.empty:
        return pd.DataFrame()

    # Per-symbol mid series indexed by ts.
    mid_by_symbol = {
        sym: _l1_mid(l1_by_symbol[sym]) for sym in held_symbols
        if sym in l1_by_symbol and not l1_by_symbol[sym].empty
    }

    # Realised PnL timeline.
    closed_events = []
    for t, pnl in zip(trips, net_pnl):
        if t.open_at_end:
            continue
        ts = t.sell_market_ts_ns if t.sell_market_ts_ns is not None else t.sell_ts_ns
        if ts is None:
            continue
        closed_events.append((ts, pnl))
    closed_events.sort()
    closed_ts = pd.Series(
        [e[1] for e in closed_events],
        index=[e[0] for e in closed_events],
    ).cumsum() if closed_events else pd.Series(dtype=float)

    rows = []
    for ts in master_ts:
        # Realised cumulative at ts: largest closed event <= ts.
        if not closed_ts.empty:
            realized = float(closed_ts.loc[:ts].iloc[-1]) \
                if (closed_ts.index <= ts).any() else 0.0
        else:
            realized = 0.0

        # Unrealized: sum of (mid_at_ts - entry) * qty for positions currently
        # open at ts (buy_ts <= ts AND (no sell yet OR sell_ts > ts)).
        unreal = 0.0
        for t in trips:
            buy_ts = t.buy_market_ts_ns
            sell_ts = t.sell_market_ts_ns
            if buy_ts is None or buy_ts > ts:
                continue
            if sell_ts is not None and sell_ts <= ts:
                continue
            mid_s = mid_by_symbol.get(t.symbol)
            if mid_s is None or mid_s.empty:
                continue
            # Latest mid at-or-before ts.
            mask = mid_s.index <= ts
            if not mask.any():
                continue
            mid_at_ts = float(mid_s.values[mask.argmax() if False else int(mask.sum()) - 1])
            unreal += (mid_at_ts - t.entry_price) * t.qty
        rows.append({"ts_ns": int(ts), "realized": realized,
                     "unrealized": unreal, "total": realized + unreal})
    df = pd.DataFrame(rows)
    df["ts"] = pd.to_datetime(df["ts_ns"], unit="ns", utc=True)
    return df


def compute_metrics(
    trips: List[RoundTrip],
    net_pnl: List[float],
    equity: pd.DataFrame,
    account_budget: float,
    *,
    l1_by_symbol: Optional[Dict[str, pd.DataFrame]] = None,
    daily_inflation_cost: float = 0.0,
) -> Dict[str, Any]:
    l1_by_symbol = l1_by_symbol or {}
    closed = [(t, pnl) for t, pnl in zip(trips, net_pnl) if not t.open_at_end]
    open_trips = [(t, pnl) for t, pnl in zip(trips, net_pnl) if t.open_at_end]

    realized = sum(pnl for _, pnl in closed)

    # Mark-to-market unrealized loss on positions still open at end of run.
    # This is the honest "bag-holding" cost the per-trade win-rate hides.
    unrealized = compute_unrealized_pnl(trips, l1_by_symbol)
    unrealized_loss_only = compute_unrealized_pnl(
        [t for t in trips if t.open_at_end and t.entry_price > 0
         and (l1_by_symbol.get(t.symbol) is not None
              and not l1_by_symbol[t.symbol].empty
              and 0.5 * (l1_by_symbol[t.symbol]["bid_price"].iat[-1]
                          + l1_by_symbol[t.symbol]["ask_price"].iat[-1])
              < t.entry_price)],
        l1_by_symbol,
    )

    n_closed = len(closed)
    n_open = len(open_trips)
    wins = [pnl for _, pnl in closed if pnl > 0]
    losses = [pnl for _, pnl in closed if pnl < 0]

    win_rate = len(wins) / n_closed if n_closed else float("nan")
    avg_win = sum(wins) / len(wins) if wins else 0.0
    avg_loss = sum(losses) / len(losses) if losses else 0.0
    gross_profit = sum(wins) if wins else 0.0
    gross_loss = -sum(losses) if losses else 0.0
    profit_factor = (
        gross_profit / gross_loss if gross_loss > 0 else float("inf") if wins else 0.0
    )

    # Prefer simulated market-time held duration over engine wall-clock so
    # Sharpe annualisation reflects real trade cadence (engine wall-clock
    # compresses a 12-trading-day window into ~1h, which would explode the
    # trades-per-year estimate).
    def holding_min(t: RoundTrip) -> float:
        m = t.holding_market_minutes
        return m if not math.isnan(m) else t.holding_minutes

    avg_holding_min = (
        sum(holding_min(t) for t, _ in closed) / n_closed if n_closed else 0.0
    )

    # Per-trade Sharpe (kept for back-compat). Sortino/Calmar are recomputed
    # below from the time-weighted equity curve, which actually contains
    # negative excursions (mark-to-market drawdowns on open positions and
    # underwater stretches on closed trades).
    if account_budget > 0 and n_closed:
        rets = [pnl / account_budget for _, pnl in closed]
        mean_r = sum(rets) / len(rets)
        var_r = sum((r - mean_r) ** 2 for r in rets) / len(rets)
        std_r = math.sqrt(var_r)
        trades_per_year = MINUTES_PER_YEAR / max(avg_holding_min, 1.0)
        sharpe = (mean_r / std_r) * math.sqrt(trades_per_year) if std_r > 0 else 0.0
    else:
        sharpe = 0.0

    # Time-weighted equity curve including mark-to-market unrealized.
    # This is the "honest" equity. max-drawdown, Sortino, Calmar use this.
    tw_equity = compute_time_weighted_equity(trips, net_pnl, l1_by_symbol)
    if not tw_equity.empty and account_budget > 0:
        total = tw_equity["total"].astype(float)
        cummax = total.cummax()
        drawdown_dollars = float((total - cummax).min())
        # Per-minute returns (in $) -> normalise by budget for ratio metrics.
        rets_eq = total.diff().dropna() / account_budget
        if len(rets_eq) > 1:
            mean_r = float(rets_eq.mean())
            std_r = float(rets_eq.std())
            downside = rets_eq[rets_eq < 0]
            dstd = float(downside.std()) if len(downside) > 1 else 0.0
            sortino = ((mean_r / dstd) * math.sqrt(MINUTES_PER_YEAR)
                       if dstd > 0 else 0.0)
            # Time-window total return as % of budget, annualised.
            window_days = (tw_equity["ts_ns"].iat[-1]
                           - tw_equity["ts_ns"].iat[0]) / 86_400_000_000_000.0
            if window_days > 0 and drawdown_dollars < 0:
                ann_return = ((realized + unrealized) / account_budget) \
                    * (365.0 / window_days)
                calmar = ann_return / (abs(drawdown_dollars) / account_budget)
            else:
                calmar = 0.0
        else:
            sortino = 0.0
            calmar = 0.0
        max_dd_dollars = drawdown_dollars
    else:
        sortino = 0.0
        calmar = 0.0
        max_dd_dollars = 0.0

    # Holding-analytics aggregates (per-trip max underwater + time below entry).
    holding = compute_holding_analytics(trips, l1_by_symbol)
    underwater_pcts = [h["max_drawdown_pct"] for h in holding
                       if h["max_drawdown_pct"] is not None]
    time_under_pcts = [h["pct_time_below_entry"] for h in holding
                       if h["pct_time_below_entry"] is not None]
    deepest_underwater = min(underwater_pcts) if underwater_pcts else 0.0
    avg_time_under = (sum(time_under_pcts) / len(time_under_pcts)
                      if time_under_pcts else 0.0)
    n_stalled_open = sum(
        1 for h in holding if h["open_at_end"]
        and h["pct_time_below_entry"] is not None
        and h["pct_time_below_entry"] >= 50.0
    )

    # Opportunity / inflation drag. AppConfig.daily_inflation_cost is an
    # ABSOLUTE dollars-per-day number (the C++ side adds it into
    # allocated_daily_cost_per_share unconditionally), not a rate. Scale
    # it up if account_budget grows: e.g. $0.15/day models ~3.7% annual on
    # a $1500 budget; $1.50/day for a $15000 budget. Zero (default) means
    # "off". We compute it as `daily_inflation_cost * days_window` so the
    # number reported here matches what the C++ cost model is already
    # accumulating per share.
    if not tw_equity.empty:
        days_window = (tw_equity["ts_ns"].iat[-1]
                       - tw_equity["ts_ns"].iat[0]) / 86_400_000_000_000.0
    else:
        days_window = 0.0
    opportunity_cost = daily_inflation_cost * days_window

    # Capital efficiency: total notional traded / (account_budget * days).
    # Anything below 1.0 means our capital is sitting more than it's working.
    total_notional = sum(t.qty * t.entry_price for t in trips)
    if account_budget > 0 and days_window > 0:
        capital_efficiency = total_notional / (account_budget * days_window)
    else:
        capital_efficiency = 0.0

    return {
        "n_round_trips_closed": n_closed,
        "n_positions_open_at_end": n_open,
        "realized_pnl_net": round(realized, 4),
        "unrealized_pnl_mark_to_market": round(unrealized, 4),
        "net_pnl_realized_plus_unrealized": round(realized + unrealized, 4),
        "win_rate": round(win_rate, 4) if not math.isnan(win_rate) else None,
        "avg_win": round(avg_win, 4),
        "avg_loss": round(avg_loss, 4),
        "gross_profit": round(gross_profit, 4),
        "gross_loss": round(gross_loss, 4),
        "deepest_drawdown_pct_any_position": round(deepest_underwater, 4),
        "avg_pct_time_below_entry": round(avg_time_under, 2),
        "n_stalled_open_positions": n_stalled_open,
        "opportunity_cost_dollars": round(opportunity_cost, 4),
        # The bottom-line "did this run actually create value?" number:
        # realized closed PnL + mark-to-market unrealized PnL - the cost
        # of the capital sitting idle over the window. Anything <= 0
        # means the strategy did not beat T-bills / inflation.
        "net_pnl_after_opportunity_cost": round(
            realized + unrealized - opportunity_cost, 4
        ),
        "capital_efficiency_ratio": round(capital_efficiency, 4),
        "profit_factor": (
            round(profit_factor, 4) if math.isfinite(profit_factor) else None
        ),
        "max_drawdown_dollars": round(max_dd_dollars, 4),
        "sharpe_ratio_annualized": round(sharpe, 4),
        "sortino_ratio_annualized": round(sortino, 4),
        "calmar_ratio": round(calmar, 4),
        "avg_holding_minutes": round(avg_holding_min, 2),
    }


# ----------------------------- Plotting ----------------------------- #


def render_equity_curve(equity: pd.DataFrame, out_path: Path) -> None:
    if equity.empty or len(equity) < 2:
        return
    fig, ax = plt.subplots(figsize=(11, 5))
    ax.plot(equity["ts"], equity["pnl_cum"], color="#1f77b4", linewidth=1.5)
    ax.scatter(
        equity["ts"][1:],
        equity["pnl_cum"][1:],
        c=["#2ca02c" if p > 0 else "#d62728" for p in equity["pnl_cum"].diff()[1:]],
        s=30,
        zorder=3,
    )
    ax.axhline(0, color="black", linewidth=0.5, alpha=0.3)
    ax.set_title("Cumulative realized PnL (net of commissions)")
    ax.set_ylabel("PnL ($)")
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%Y-%m-%d %H:%M"))
    fig.autofmt_xdate()
    fig.tight_layout()
    fig.savefig(out_path, dpi=120)
    plt.close(fig)


def render_per_trade_pnl(net_pnl: List[float], trips: List[RoundTrip],
                         out_path: Path) -> None:
    closed = [(t, pnl) for t, pnl in zip(trips, net_pnl) if not t.open_at_end]
    if not closed:
        return
    labels = [f"{t.symbol}\n#{i+1}" for i, (t, _) in enumerate(closed)]
    pnls = [pnl for _, pnl in closed]
    colors = ["#2ca02c" if p > 0 else "#d62728" for p in pnls]
    fig, ax = plt.subplots(figsize=(max(6, 0.6 * len(labels)), 4))
    ax.bar(labels, pnls, color=colors)
    ax.axhline(0, color="black", linewidth=0.5)
    ax.set_title("Per-trade net PnL")
    ax.set_ylabel("$")
    fig.tight_layout()
    fig.savefig(out_path, dpi=120)
    plt.close(fig)


def render_symbol_plot(
    symbol: str,
    trips: List[RoundTrip],
    step_trace: pd.DataFrame,
    l1_df: Optional[pd.DataFrame],
    target_profit_pct: float,
    out_path: Path,
) -> None:
    fig, ax = plt.subplots(figsize=(11, 5))

    # Prefer L1 CSV (full resolution) for the price line; fall back to
    # step_trace's mid for this symbol.
    if l1_df is not None and not l1_df.empty:
        l1_df = l1_df.copy()
        l1_df["mid"] = 0.5 * (l1_df["bid_price"] + l1_df["ask_price"])
        l1_df["ts"] = pd.to_datetime(l1_df["ts_event"], unit="ns", utc=True)
        ax.plot(l1_df["ts"], l1_df["mid"], color="#1f77b4", linewidth=0.7,
                label="L1 mid")
    elif not step_trace.empty:
        s_rows = step_trace[step_trace["symbol"] == symbol]
        if not s_rows.empty:
            ts = pd.to_datetime(s_rows["ts_ns"], unit="ns", utc=True)
            ax.plot(ts, s_rows["mid"], color="#1f77b4", linewidth=0.7,
                    label="step_trace mid")

    sym_trips = [t for t in trips if t.symbol == symbol]
    for t in sym_trips:
        # Prefer the simulated market time; fall back to engine wall-clock.
        buy_ns = t.buy_market_ts_ns if t.buy_market_ts_ns is not None else t.buy_ts_ns
        buy_ts = pd.to_datetime(buy_ns, unit="ns", utc=True)
        ax.scatter([buy_ts], [t.entry_price], marker="^", color="#2ca02c",
                   s=80, zorder=5, label="buy fill" if t is sym_trips[0] else None)
        # Sell-target horizontal line from buy to sell (or end of plot).
        target = t.entry_price * (1.0 + target_profit_pct)
        if t.sell_ts_ns is not None:
            sell_ns = (t.sell_market_ts_ns if t.sell_market_ts_ns is not None
                       else t.sell_ts_ns)
            sell_ts = pd.to_datetime(sell_ns, unit="ns", utc=True)
            ax.hlines(target, buy_ts, sell_ts, color="#ff7f0e", linewidth=1.0,
                      linestyles="dashed")
            ax.scatter(
                [sell_ts],
                [t.exit_price],
                marker="v",
                color="#2ca02c" if (t.exit_price - t.entry_price) > 0 else "#d62728",
                s=80,
                zorder=5,
                label="sell fill" if t is sym_trips[0] else None,
            )
        else:
            # Open at end: dashed target to right edge.
            ax.axhline(target, color="#ff7f0e", linewidth=1.0,
                       linestyle="dashed", alpha=0.5)
            ax.scatter([buy_ts], [t.entry_price], marker="s",
                       facecolors="none", edgecolors="#888", s=120, zorder=4,
                       label="open at end" if t is sym_trips[0] else None)

    ax.set_title(f"{symbol} — price + entry/exit")
    ax.set_ylabel("Price ($)")
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%Y-%m-%d %H:%M"))
    ax.legend(loc="best", fontsize=8)
    fig.autofmt_xdate()
    fig.tight_layout()
    fig.savefig(out_path, dpi=120)
    plt.close(fig)


# ----------------------------- Reporting ---------------------------- #


def write_metrics_markdown(metrics: Dict[str, Any], trips: List[RoundTrip],
                            net_pnl: List[float], cfg: Dict[str, str],
                            out_path: Path) -> None:
    lines: List[str] = []
    label = cfg.get("run_label", "")
    window = (
        f"{cfg.get('databento_start', '?')} -> {cfg.get('databento_end', '?')}"
    )
    lines.append(f"# Backtest metrics — {label}")
    lines.append("")
    lines.append(f"**Window:** {window}")
    lines.append(f"**Universe size:** {cfg.get('universe_size', '?')}, "
                 f"top_k={cfg.get('top_k', '?')}, "
                 f"max_open_symbols={cfg.get('max_open_symbols', '?')}")
    lines.append("")
    lines.append("## Summary")
    lines.append("")
    lines.append("| Metric | Value |")
    lines.append("|---|---|")
    for k, v in metrics.items():
        lines.append(f"| {k} | {v} |")
    lines.append("")
    lines.append("## Round-trips (closed)")
    lines.append("")
    lines.append("| # | Symbol | Entry | Exit | Qty | Net PnL ($) | Held (market min) |")
    lines.append("|---|---|---|---|---|---|---|")
    for i, (t, pnl) in enumerate(zip(trips, net_pnl)):
        if t.open_at_end:
            continue
        held = (
            t.holding_market_minutes
            if not math.isnan(t.holding_market_minutes)
            else t.holding_minutes
        )
        lines.append(
            f"| {i+1} | {t.symbol} | {t.entry_price:.4f} | "
            f"{t.exit_price:.4f} | {t.qty:g} | {pnl:+.4f} | "
            f"{held:.1f} |"
        )
    open_trips = [(t, pnl) for t, pnl in zip(trips, net_pnl) if t.open_at_end]
    if open_trips:
        lines.append("")
        lines.append("## Open positions at end")
        lines.append("")
        lines.append("| Symbol | Entry | Qty | Notional ($) |")
        lines.append("|---|---|---|---|")
        for t, _ in open_trips:
            lines.append(
                f"| {t.symbol} | {t.entry_price:.4f} | {t.qty:g} | "
                f"{t.entry_price * t.qty:.2f} |"
            )
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


# ----------------------------- Driver ------------------------------- #


def main(argv: List[str]) -> int:
    p = argparse.ArgumentParser()
    p.add_argument("run_dir", type=Path,
                   help="Path to reports/runs/<run_id>")
    p.add_argument("--l1-dir", type=Path, default=Path("data/l1"),
                   help="Where to find <SYM>.mbp1.csv for per-symbol price.")
    args = p.parse_args(argv)

    run_dir: Path = args.run_dir
    if not run_dir.is_dir():
        print(f"plot_run: not a directory: {run_dir}", file=sys.stderr)
        return 2

    plots_dir = run_dir / "plots"
    plots_dir.mkdir(parents=True, exist_ok=True)

    orders = read_csv_skipping_comments(run_dir / "orders.csv")
    step_trace = read_csv_skipping_comments(run_dir / "step_trace.csv")
    cfg = read_config(run_dir)

    target_profit_pct = float(cfg.get("target_profit_pct", 0.008))
    # IBKR Pro Tiered defaults (matches the C++ AppConfig defaults from
    # 2026-05-21). The config key the engine actually reads is
    # `commission_min_per_order`; we accept the older `min_per_order` too
    # as a fallback so any pre-existing manifests still load cleanly.
    commission_per_share = float(cfg.get("commission_per_share", 0.0035))
    min_per_order = float(
        cfg.get("commission_min_per_order", cfg.get("min_per_order", 0.35))
    )
    account_budget = float(cfg.get("account_budget", 1500.0))
    # Daily inflation cost from AppConfig.costs (0 by default = the metric
    # is off). Used in opportunity_cost = budget * rate * days_in_window.
    daily_inflation_cost = float(cfg.get("daily_inflation_cost", 0.0))

    trips = derive_round_trips(orders, commission_per_share, min_per_order)
    if not trips:
        print("plot_run: no trades found in orders.csv", file=sys.stderr)
        return 0

    # Preload per-symbol L1 frames once; reused for marker timestamps AND
    # per-symbol plotting AND the mark-to-market / holding-analytics path
    # (need full L1 mid trajectory across each holding window).
    #
    # Layout is dated-per-window (see include/broker/cache_filename.hpp):
    #   <l1_dir>/<startDate>_<endDate>/<SYM>_<startISO>_<endISO>.mbp1.csv
    # We pick the file whose ts_event range COVERS the run's configured
    # window so we don't accidentally read a different window's L1 (e.g.
    # both Yen 2024-08 and 2026q2 are in data/l1/ side by side; a naive
    # glob sorts alphabetically and would pick the older one). Falls back
    # to the legacy flat <l1_dir>/<SYM>.mbp1.csv last.
    symbols = sorted({t.symbol for t in trips})
    l1_by_symbol: Dict[str, pd.DataFrame] = {}

    cfg_start_iso = cfg.get("databento_start", "")
    cfg_end_iso = cfg.get("databento_end", "")
    cfg_start_ns: Optional[int] = None
    cfg_end_ns: Optional[int] = None
    try:
        if cfg_start_iso:
            cfg_start_ns = int(
                pd.Timestamp(cfg_start_iso.replace("Z", "+00:00")).value
            )
        if cfg_end_iso:
            cfg_end_ns = int(
                pd.Timestamp(cfg_end_iso.replace("Z", "+00:00")).value
            )
    except Exception:
        pass

    def pick_l1(sym: str) -> Optional[Path]:
        candidates = sorted(args.l1_dir.glob(f"**/{sym}_*.mbp1.csv"))
        if cfg_start_ns is not None and cfg_end_ns is not None:
            # Prefer files whose ts_event range covers the run window.
            # The filename has it: SYM_<startISO>_<endISO>.mbp1.csv with
            # basic ISO 8601 (YYYYMMDDTHHMMSSZ).
            for p in candidates:
                name = p.stem  # drops ".mbp1.csv" leaving SYM_<s>_<e>.mbp1
                # leaf without ".mbp1" -> e.g. LRCX_20260413T133000Z_20260428T195900Z
                core = name[:-5] if name.endswith(".mbp1") else name
                parts = core.rsplit("_", 2)
                if len(parts) < 3:
                    continue
                start_str, end_str = parts[1], parts[2]
                try:
                    s = int(pd.Timestamp(
                        f"{start_str[:4]}-{start_str[4:6]}-{start_str[6:8]}T"
                        f"{start_str[9:11]}:{start_str[11:13]}:{start_str[13:15]}+00:00"
                    ).value)
                    e = int(pd.Timestamp(
                        f"{end_str[:4]}-{end_str[4:6]}-{end_str[6:8]}T"
                        f"{end_str[9:11]}:{end_str[11:13]}:{end_str[13:15]}+00:00"
                    ).value)
                except Exception:
                    continue
                # 24h end-tolerance mirrors broker's cache_covers_window.
                if s <= cfg_start_ns + 60 * 1_000_000_000 and \
                   e >= cfg_end_ns - 24 * 60 * 60 * 1_000_000_000:
                    return p
        if candidates:
            return candidates[0]
        legacy = args.l1_dir / f"{sym}.mbp1.csv"
        return legacy if legacy.exists() else None

    for sym in symbols:
        path = pick_l1(sym)
        if path is not None:
            l1_by_symbol[sym] = pd.read_csv(path)

    attach_market_timestamps(trips, l1_by_symbol)

    net_pnl = apply_commissions(trips, commission_per_share, min_per_order)
    equity = build_equity_curve(trips, net_pnl)
    metrics = compute_metrics(
        trips, net_pnl, equity, account_budget,
        l1_by_symbol=l1_by_symbol,
        daily_inflation_cost=daily_inflation_cost,
    )

    # Plots.
    render_equity_curve(equity, plots_dir / "equity_curve.png")
    render_per_trade_pnl(net_pnl, trips, plots_dir / "pnl_per_trade.png")

    for sym in symbols:
        render_symbol_plot(
            symbol=sym,
            trips=trips,
            step_trace=step_trace,
            l1_df=l1_by_symbol.get(sym),
            target_profit_pct=target_profit_pct,
            out_path=plots_dir / f"{sym}.png",
        )

    (run_dir / "metrics.json").write_text(
        json.dumps(metrics, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    write_metrics_markdown(metrics, trips, net_pnl, cfg, run_dir / "metrics.md")

    print(f"plot_run: wrote {len(symbols)} symbol plot(s) + metrics to {run_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
