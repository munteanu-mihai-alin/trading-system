#!/usr/bin/env python3
"""QuantConnect-format backtest report emitter.

Transforms a completed run folder (the CSVs `hft_app` writes, plus the
metrics `plot_run.py` computes) into a JSON document shaped like a
QuantConnect LEAN backtest result:

    reports/runs/<run_id>/qc_result.json

The point is a single, familiar schema the mobile app can render the
same way for every strategy/branch. We map what we can 1:1 to QC's
sections and stash EVERYTHING our own pipeline produces under a
namespaced `hftExtra` key, so switching to the QC shape never drops a
metric we already collect.

Fields we can't honestly produce are omitted rather than faked, and
listed in `hftExtra.qcOmitted` with the reason:
  - Alpha / Beta / InformationRatio / TrackingError / TreynorRatio and
    the Benchmark chart need a benchmark price series (QC uses SPY);
    we don't track one yet.
  - Strategy Capacity is QC's proprietary liquidity model.

Usage:
  python scripts/qc_report.py reports/runs/<run_id>
  python scripts/qc_report.py reports/runs/<run_id> --l1-dir data/l1

Reuses plot_run.py's round-trip / equity / L1 machinery so the numbers
match metrics.json exactly.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional

import pandas as pd

# Reuse the exact derivation plot_run.py uses so qc_result.json and
# metrics.json can never disagree.
from plot_run import (
    RoundTrip,
    apply_commissions,
    build_equity_curve,
    compute_metrics,
    derive_round_trips,
    read_config,
    read_csv_skipping_comments,
)

TRADING_DAYS_PER_YEAR = 252


# --------------------------- small helpers -------------------------- #


def _iso(ts_ns: Optional[int]) -> Optional[str]:
    if ts_ns is None:
        return None
    return pd.to_datetime(int(ts_ns), unit="ns", utc=True).strftime(
        "%Y-%m-%dT%H:%M:%SZ"
    )


def _pct(x: float, digits: int = 3) -> str:
    """QC renders ratios as percent strings, e.g. '56.840%'."""
    return f"{x * 100:.{digits}f}%"


def _usd(x: float) -> str:
    return f"${x:,.2f}"


def _unix(ts_ns: int) -> int:
    return int(int(ts_ns) // 1_000_000_000)


# ------------------------ per-trade MFE/MAE ------------------------- #


def compute_excursions(
    trips: List[RoundTrip],
    l1_by_symbol: Dict[str, pd.DataFrame],
) -> List[Dict[str, Any]]:
    """Per-trip max adverse (MAE) and max favorable (MFE) excursion in
    dollars over the holding window, plus entry/exit dollars. plot_run
    already computes the adverse side for its holding analytics; here we
    add the favorable peak so we can fill QC's tradeStatistics."""
    from plot_run import _l1_mid

    out: List[Dict[str, Any]] = []
    for t in trips:
        rec: Dict[str, Any] = {
            "symbol": t.symbol,
            "mae_dollars": 0.0,
            "mfe_dollars": 0.0,
        }
        l1 = l1_by_symbol.get(t.symbol)
        if l1 is None or l1.empty or t.buy_market_ts_ns is None:
            out.append(rec)
            continue
        mid = _l1_mid(l1)
        start_ns = t.buy_market_ts_ns
        end_ns = (
            t.sell_market_ts_ns
            if t.sell_market_ts_ns is not None
            else int(l1["ts_event"].max())
        )
        window = mid[(mid.index >= start_ns) & (mid.index <= end_ns)]
        if window.empty:
            out.append(rec)
            continue
        lo = float(window.min())
        hi = float(window.max())
        rec["mae_dollars"] = round((lo - t.entry_price) * t.qty, 2)
        rec["mfe_dollars"] = round((hi - t.entry_price) * t.qty, 2)
        out.append(rec)
    return out


# --------------------------- QC sections ---------------------------- #


def build_orders(orders: pd.DataFrame) -> Dict[str, Any]:
    """QC `orders` map, keyed by stringified order id. We emit one entry
    per filled order leg with the fields our CSV carries; QC-only fields
    (contingentId, permtick, securityType codes) are stubbed with the
    values LEAN uses for a filled US equity market/limit order."""
    if orders is None or orders.empty:
        return {}
    filled = orders[orders["event"] == "filled"].copy()
    if filled.empty:
        return {}
    filled.sort_values("ts_ns", inplace=True)

    # The engine's orders.csv names the limit column `limit`; older/other
    # emitters used `limit_price`. Accept either.
    limit_col = "limit" if "limit" in filled.columns else (
        "limit_price" if "limit_price" in filled.columns else None
    )
    have_limit = limit_col is not None
    have_bid = "bid_price" in filled.columns
    have_ask = "ask_price" in filled.columns

    out: Dict[str, Any] = {}
    for _, row in filled.iterrows():
        oid = int(row.get("order_id", len(out) + 1))
        side = str(row["side"])
        qty = float(row["filled_qty"])
        price = float(row["avg_fill_price"])
        signed_qty = qty if side == "buy" else -qty
        ts = _iso(int(row["ts_ns"]))
        is_limit = have_limit and not pd.isna(row.get(limit_col))
        entry = {
            "type": 1 if is_limit else 0,          # 1=limit, 0=market
            "id": oid,
            "contingentId": 0,
            "brokerId": [str(oid)],
            "symbol": {
                "value": row["symbol"],
                "id": row["symbol"],
                "permtick": row["symbol"],
            },
            "price": round(price, 6),
            "priceCurrency": "USD",
            "time": ts,
            "createdTime": ts,
            "lastFillTime": ts,
            "quantity": signed_qty,
            "status": 3,                            # filled
            "tag": "",
            "properties": {"timeInForce": {}},
            "securityType": 1,                      # equity
            "direction": 0 if side == "buy" else 1,
            "value": round(price * signed_qty, 4),
            "orderSubmissionData": {
                "bidPrice": round(float(row["bid_price"]), 6) if have_bid else price,
                "askPrice": round(float(row["ask_price"]), 6) if have_ask else price,
                "lastPrice": price,
            },
            "isMarketable": not is_limit,
            "priceAdjustmentMode": 0,
        }
        if is_limit:
            entry["limitPrice"] = round(float(row[limit_col]), 6)
        out[str(oid)] = entry
    return out


def build_profit_loss(
    trips: List[RoundTrip], net_pnl: List[float]
) -> Dict[str, float]:
    """QC `profitLoss`: realized P&L keyed by the ISO timestamp of each
    closing trade."""
    out: Dict[str, float] = {}
    for t, pnl in zip(trips, net_pnl):
        if t.open_at_end:
            continue
        ts_ns = (
            t.sell_market_ts_ns
            if t.sell_market_ts_ns is not None
            else t.sell_ts_ns
        )
        key = _iso(ts_ns)
        if key is not None:
            out[key] = round(pnl, 2)
    return out


def build_closed_trades(
    trips: List[RoundTrip],
    net_pnl: List[float],
    excursions: List[Dict[str, Any]],
) -> List[Dict[str, Any]]:
    """QC totalPerformance.closedTrades entries."""
    out: List[Dict[str, Any]] = []
    for t, pnl, exc in zip(trips, net_pnl, excursions):
        if t.open_at_end:
            continue
        entry_ns = t.buy_market_ts_ns or t.buy_ts_ns
        exit_ns = t.sell_market_ts_ns or t.sell_ts_ns
        duration_s = (
            (int(exit_ns) - int(entry_ns)) / 1e9
            if entry_ns and exit_ns
            else 0.0
        )
        out.append({
            "symbol": {"value": t.symbol, "id": t.symbol, "permtick": t.symbol},
            "entryTime": _iso(entry_ns),
            "entryPrice": round(t.entry_price, 6),
            "exitTime": _iso(exit_ns),
            "exitPrice": round(t.exit_price, 6) if t.exit_price else None,
            "quantity": t.qty,
            "direction": 0,                        # long-only strategy
            "profitLoss": round(pnl, 2),
            "totalFees": 0.0,                       # folded into net pnl
            "mae": exc["mae_dollars"],
            "mfe": exc["mfe_dollars"],
            "durationSeconds": round(duration_s, 1),
        })
    return out


def build_trade_statistics(
    trips: List[RoundTrip],
    net_pnl: List[float],
    excursions: List[Dict[str, Any]],
) -> Dict[str, Any]:
    closed = [
        (t, pnl, e)
        for t, pnl, e in zip(trips, net_pnl, excursions)
        if not t.open_at_end
    ]
    n = len(closed)
    if n == 0:
        return {"totalNumberOfTrades": 0}
    pnls = [pnl for _, pnl, _ in closed]
    wins = [p for p in pnls if p > 0]
    losses = [p for p in pnls if p < 0]
    durations = []
    for t, _, _ in closed:
        entry_ns = t.buy_market_ts_ns or t.buy_ts_ns
        exit_ns = t.sell_market_ts_ns or t.sell_ts_ns
        if entry_ns and exit_ns:
            durations.append((int(exit_ns) - int(entry_ns)) / 1e9)
    maes = [e["mae_dollars"] for _, _, e in closed]
    mfes = [e["mfe_dollars"] for _, _, e in closed]

    # Longest consecutive win/loss streak.
    def max_streak(pred) -> int:
        best = cur = 0
        for p in pnls:
            if pred(p):
                cur += 1
                best = max(best, cur)
            else:
                cur = 0
        return best

    gross_profit = sum(wins)
    gross_loss = -sum(losses)
    return {
        "totalNumberOfTrades": n,
        "numberOfWinningTrades": len(wins),
        "numberOfLosingTrades": len(losses),
        "totalProfitLoss": round(sum(pnls), 2),
        "totalProfit": round(gross_profit, 2),
        "totalLoss": round(-gross_loss, 2),
        "largestProfit": round(max(wins), 2) if wins else 0.0,
        "largestLoss": round(min(losses), 2) if losses else 0.0,
        "averageProfitLoss": round(sum(pnls) / n, 4),
        "averageProfit": round(gross_profit / len(wins), 4) if wins else 0.0,
        "averageLoss": round(-gross_loss / len(losses), 4) if losses else 0.0,
        "averageTradeDurationSeconds": round(sum(durations) / len(durations), 1)
        if durations else 0.0,
        "maxConsecutiveWinningTrades": max_streak(lambda p: p > 0),
        "maxConsecutiveLosingTrades": max_streak(lambda p: p < 0),
        "winRate": round(len(wins) / n, 4),
        "lossRate": round(len(losses) / n, 4),
        "profitFactor": round(gross_profit / gross_loss, 4)
        if gross_loss > 0 else None,
        "averageMAE": round(sum(maes) / len(maes), 4) if maes else 0.0,
        "averageMFE": round(sum(mfes) / len(mfes), 4) if mfes else 0.0,
        "largestMAE": round(min(maes), 2) if maes else 0.0,
        "largestMFE": round(max(mfes), 2) if mfes else 0.0,
    }


def build_statistics(
    metrics: Dict[str, Any],
    trips: List[RoundTrip],
    net_pnl: List[float],
    account_budget: float,
    total_fees: float,
    window_days: float,
) -> Dict[str, str]:
    """QC top-level `statistics` (string-formatted, as LEAN emits)."""
    closed = [(t, p) for t, p in zip(trips, net_pnl) if not t.open_at_end]
    n_open = sum(1 for t in trips if t.open_at_end)
    total_orders = 2 * len(closed) + n_open

    realized = metrics.get("realized_pnl_net", 0.0) or 0.0
    unrealized = metrics.get("unrealized_pnl_mark_to_market", 0.0) or 0.0
    net = realized + unrealized
    start_eq = account_budget
    end_eq = account_budget + net
    net_profit = (net / start_eq) if start_eq else 0.0

    # Per-trade % returns on notional, for QC's average win/loss.
    def pct_ret(t: RoundTrip, pnl: float) -> float:
        notional = t.entry_price * t.qty
        return pnl / notional if notional else 0.0

    win_rets = [pct_ret(t, p) for t, p in closed if p > 0]
    loss_rets = [pct_ret(t, p) for t, p in closed if p < 0]
    win_rate = metrics.get("win_rate")

    ann_return = (
        net_profit * (365.0 / window_days) if window_days > 0 else 0.0
    )
    max_dd = metrics.get("max_drawdown_dollars", 0.0) or 0.0
    drawdown_pct = abs(max_dd) / start_eq if start_eq else 0.0

    return {
        "Total Orders": str(total_orders),
        "Average Win": _pct(sum(win_rets) / len(win_rets)) if win_rets else "0%",
        "Average Loss": _pct(sum(loss_rets) / len(loss_rets)) if loss_rets else "0%",
        "Compounding Annual Return": _pct(ann_return),
        "Drawdown": _pct(drawdown_pct),
        "Expectancy": str(round(metrics.get("profit_factor") or 0.0, 3)),
        "Start Equity": _usd(start_eq),
        "End Equity": _usd(end_eq),
        "Net Profit": _pct(net_profit),
        "Sharpe Ratio": str(round(metrics.get("sharpe_ratio_annualized", 0.0) or 0.0, 3)),
        "Sortino Ratio": str(round(metrics.get("sortino_ratio_annualized", 0.0) or 0.0, 3)),
        "Win Rate": _pct(win_rate) if win_rate is not None else "0%",
        "Loss Rate": _pct(1 - win_rate) if win_rate is not None else "0%",
        "Profit-Loss Ratio": str(round(metrics.get("profit_factor") or 0.0, 3)),
        "Total Fees": _usd(total_fees),
        "Calmar Ratio": str(round(metrics.get("calmar_ratio", 0.0) or 0.0, 3)),
        "Portfolio Turnover": _pct(metrics.get("capital_efficiency_ratio", 0.0) or 0.0),
    }


def build_charts(
    equity: pd.DataFrame,
    tw_equity: pd.DataFrame,
    account_budget: float,
) -> Dict[str, Any]:
    """QC `charts`: Strategy Equity (Equity + Return) + Drawdown, as
    series of {x: unixSeconds, y: value}. Uses the time-weighted equity
    (realized + unrealized) when available, else the per-trip curve."""

    def series(points: List[Dict[str, float]], name: str, unit: str) -> Dict[str, Any]:
        return {"name": name, "unit": unit, "values": points}

    charts: Dict[str, Any] = {}
    src = tw_equity if not tw_equity.empty else equity
    if src.empty:
        return charts

    if not tw_equity.empty:
        eq_points = [
            {"x": _unix(r.ts_ns), "y": round(account_budget + r.total, 2)}
            for r in tw_equity.itertuples()
        ]
        ret_points = [
            {"x": _unix(r.ts_ns), "y": round(r.total / account_budget, 6)}
            for r in tw_equity.itertuples()
        ]
        total = tw_equity["total"].astype(float)
        cummax = total.cummax()
        dd_points = [
            {"x": _unix(ts), "y": round(float(d) / account_budget, 6)}
            for ts, d in zip(tw_equity["ts_ns"], (total - cummax))
        ]
    else:
        eq_points = [
            {"x": _unix(r.ts_ns), "y": round(account_budget + r.pnl_cum, 2)}
            for r in equity.itertuples()
        ]
        ret_points = [
            {"x": _unix(r.ts_ns), "y": round(r.pnl_cum / account_budget, 6)}
            for r in equity.itertuples()
        ]
        cum = equity["pnl_cum"].astype(float)
        cummax = cum.cummax()
        dd_points = [
            {"x": _unix(ts), "y": round(float(d) / account_budget, 6)}
            for ts, d in zip(equity["ts_ns"], (cum - cummax))
        ]

    charts["Strategy Equity"] = {
        "name": "Strategy Equity",
        "series": {
            "Equity": series(eq_points, "Equity", "$"),
            "Return": series(ret_points, "Return", "%"),
        },
    }
    charts["Drawdown"] = {
        "name": "Drawdown",
        "series": {"Equity Drawdown": series(dd_points, "Equity Drawdown", "%")},
    }
    return charts


# ------------------------------ driver ------------------------------ #


def assemble(
    *,
    trips: List[RoundTrip],
    net_pnl: List[float],
    metrics: Dict[str, Any],
    equity: pd.DataFrame,
    tw_equity: pd.DataFrame,
    l1_by_symbol: Dict[str, pd.DataFrame],
    cfg: Dict[str, str],
    orders: pd.DataFrame,
) -> Dict[str, Any]:
    """Pure assembly of the QC document from already-computed inputs. No
    heavy recomputation -- callers (plot_run.main, or build_qc_result)
    pass in what they already have so qc_result.json and metrics.json
    can never disagree and we never redo the O(n*m) equity loop."""
    commission_per_share = float(cfg.get("commission_per_share", 0.0035))
    min_per_order = float(
        cfg.get("commission_min_per_order", cfg.get("min_per_order", 0.35))
    )
    account_budget = float(cfg.get("account_budget", 1500.0))

    excursions = compute_excursions(trips, l1_by_symbol)

    # Total commissions across all legs (buy always; sell only if closed).
    total_fees = 0.0
    for t in trips:
        total_fees += max(commission_per_share * t.qty, min_per_order)
        if not t.open_at_end:
            total_fees += max(commission_per_share * t.qty, min_per_order)

    # Window + start/end come from whichever equity series we have.
    src = tw_equity if not tw_equity.empty else equity
    ts_col = "ts_ns"
    if not src.empty and ts_col in src.columns and len(src) > 1:
        start_ns = int(src[ts_col].iat[0])
        end_ns = int(src[ts_col].iat[-1])
        window_days = (end_ns - start_ns) / 86_400_000_000_000.0
    else:
        start_ns = end_ns = None
        window_days = 0.0

    realized = metrics.get("realized_pnl_net", 0.0) or 0.0
    unrealized = metrics.get("unrealized_pnl_mark_to_market", 0.0) or 0.0
    net = realized + unrealized
    name = cfg.get("run_label") or "run"
    result_orders = build_orders(orders)

    result: Dict[str, Any] = {
        "algorithmConfiguration": {
            "name": name,
            "accountCurrency": "USD",
            "startDate": cfg.get("databento_start"),
            "endDate": cfg.get("databento_end"),
            "tradingDaysPerYear": TRADING_DAYS_PER_YEAR,
            "parameters": {
                k: cfg[k]
                for k in (
                    "target_profit_pct", "trade_notional", "account_budget",
                    "universe_size", "top_k", "strategy_mode",
                    "entry_limit_mode",
                )
                if k in cfg
            },
        },
        "statistics": build_statistics(
            metrics, trips, net_pnl, account_budget, total_fees, window_days
        ),
        "runtimeStatistics": {
            "Equity": _usd(account_budget + net),
            "Fees": f"-{_usd(total_fees)}",
            "Holdings": _usd(sum(
                t.qty * t.entry_price for t in trips if t.open_at_end
            )),
            "Net Profit": _usd(net),
            "Return": _pct(net / account_budget if account_budget else 0.0),
            "Unrealized": _usd(unrealized),
            "Volume": _usd(sum(t.qty * t.entry_price for t in trips)),
        },
        "orders": result_orders,
        "profitLoss": build_profit_loss(trips, net_pnl),
        "charts": build_charts(equity, tw_equity, account_budget),
        "totalPerformance": {
            "tradeStatistics": build_trade_statistics(trips, net_pnl, excursions),
            "closedTrades": build_closed_trades(trips, net_pnl, excursions),
        },
        "state": {
            "StartTime": _iso(start_ns),
            "EndTime": _iso(end_ns),
            "OrderCount": len(result_orders),
            "Name": name,
            "Status": "Completed",
        },
        # Everything our own pipeline produces, verbatim -- switching to
        # the QC shape never loses a metric we already collect.
        "hftExtra": {
            "metrics": metrics,
            "qcOmitted": {
                "Alpha/Beta/InformationRatio/TrackingError/TreynorRatio/Benchmark":
                    "needs a benchmark price series (SPY); not tracked yet",
                "StrategyCapacity": "QC proprietary liquidity model; not reproduced",
                "ProbabilisticSharpeRatio": "derivable from return skew/kurtosis; TODO",
            },
        },
    }
    return result


def build_qc_result(
    run_dir: Path, l1_dir: Path, *, load_l1: bool = True
) -> Dict[str, Any]:
    """Standalone builder: regenerate qc_result.json from a finished run
    without re-running the whole plot_run pipeline.

    Fast path: reads the existing metrics.json (written by plot_run) for
    all ratio metrics, and uses the per-trip realized equity curve for
    charts -- avoiding the O(n*m) time-weighted equity loop. L1 is loaded
    only for MAE/MFE excursions; pass load_l1=False to skip it (excursions
    then come back as zero) when speed matters more than MAE/MFE.
    """
    orders = read_csv_skipping_comments(run_dir / "orders.csv")
    cfg = read_config(run_dir)
    commission_per_share = float(cfg.get("commission_per_share", 0.0035))
    min_per_order = float(
        cfg.get("commission_min_per_order", cfg.get("min_per_order", 0.35))
    )
    account_budget = float(cfg.get("account_budget", 1500.0))

    trips = derive_round_trips(orders, commission_per_share, min_per_order)
    net_pnl = apply_commissions(trips, commission_per_share, min_per_order)

    l1_by_symbol: Dict[str, pd.DataFrame] = {}
    if load_l1 and trips:
        from plot_run import attach_market_timestamps
        for sym in sorted({t.symbol for t in trips}):
            matches = sorted(l1_dir.glob(f"**/{sym}_*.mbp1.csv"))
            if not matches:
                legacy = l1_dir / f"{sym}.mbp1.csv"
                if legacy.exists():
                    matches = [legacy]
            if matches:
                l1_by_symbol[sym] = pd.read_csv(matches[0])
        attach_market_timestamps(trips, l1_by_symbol)

    # Prefer the metrics.json plot_run already wrote; recompute only if it
    # is missing (keeps this fast -- no time-weighted equity loop here).
    metrics_path = run_dir / "metrics.json"
    if metrics_path.exists():
        metrics = json.loads(metrics_path.read_text())
    else:
        metrics = compute_metrics(
            trips, net_pnl, build_equity_curve(trips, net_pnl),
            account_budget, l1_by_symbol=l1_by_symbol, orders=orders,
        )

    equity = build_equity_curve(trips, net_pnl)
    empty = pd.DataFrame()
    return assemble(
        trips=trips, net_pnl=net_pnl, metrics=metrics,
        equity=equity, tw_equity=empty, l1_by_symbol=l1_by_symbol,
        cfg=cfg, orders=orders,
    )


def main(argv: List[str]) -> int:
    p = argparse.ArgumentParser()
    p.add_argument("run_dir", type=Path, help="Path to reports/runs/<run_id>")
    p.add_argument("--l1-dir", type=Path, default=Path("data/l1"))
    p.add_argument("--no-l1", action="store_true",
                   help="Skip L1 load (MAE/MFE come back zero, much faster)")
    p.add_argument("--out", type=Path, default=None,
                   help="Output path (default: <run_dir>/qc_result.json)")
    args = p.parse_args(argv)

    if not args.run_dir.is_dir():
        print(f"qc_report: not a directory: {args.run_dir}", file=sys.stderr)
        return 2

    result = build_qc_result(args.run_dir, args.l1_dir, load_l1=not args.no_l1)
    out = args.out or (args.run_dir / "qc_result.json")
    out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"qc_report: wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
