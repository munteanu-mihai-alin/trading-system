#!/usr/bin/env python3
"""Print a JSON snapshot of positions, working orders and P&L from IBKR.

Run as a subprocess by scripts/backend/api.py rather than imported.
ib_insync drives its own asyncio loop, and calling its sync API from
inside uvicorn's running loop deadlocks; a subprocess keeps the two
apart and also means a hung gateway cannot wedge the API.

Exists because /live/orders used to read reports/orders.csv and
reports/decisions.csv, and the Chronos engine writes NEITHER. The
config still carries order_log_path and decision_log_path, but nothing
on this branch consumes them -- they belonged to LiveExecutionEngine,
which was deleted. So the app showed an empty orders screen while the
paper account held six positions.

Portfolio data (marketPrice, unrealizedPNL) comes from the account
feed, not a market-data line, so this does not consume one of the
single-session quote entitlements and cannot trigger error 10197.

Usage: ibkr_snapshot.py [--host H] [--port P] [--client-id N]
"""
import argparse
import json
import sys


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=4002)
    # Must differ from the engine's client_id (1) or the gateway drops
    # one of the two connections.
    ap.add_argument("--client-id", type=int, default=7)
    ap.add_argument("--timeout", type=float, default=15.0)
    args = ap.parse_args()

    out = {"ok": False, "positions": [], "orders": [], "account": {},
           "error": None}
    try:
        from ib_insync import IB
    except ImportError as exc:
        out["error"] = f"ib_insync unavailable: {exc}"
        print(json.dumps(out))
        return 0

    ib = IB()
    try:
        ib.connect(args.host, args.port, clientId=args.client_id,
                   timeout=args.timeout)
    except Exception as exc:
        out["error"] = f"connect failed: {str(exc)[:200]}"
        print(json.dumps(out))
        return 0

    try:
        ib.reqAllOpenOrders()
        ib.sleep(2.0)

        for p in ib.portfolio():
            out["positions"].append({
                "symbol": p.contract.symbol,
                "qty": float(p.position),
                "avg_cost": float(p.averageCost),
                "market_price": float(p.marketPrice),
                "market_value": float(p.marketValue),
                "unrealized": float(p.unrealizedPNL),
                "realized": float(p.realizedPNL),
            })

        for t in ib.openOrders():
            out["orders"].append({
                "order_id": t.orderId,
                "symbol": getattr(t, "symbol", None),
                "side": t.action,
                "qty": float(t.totalQuantity),
                "limit": float(t.lmtPrice) if t.lmtPrice else None,
                "tif": t.tif,
            })
        # openOrders() returns Order objects without the contract, so
        # recover symbols from the matching trades.
        by_id = {t.order.orderId: t.contract.symbol for t in ib.trades()}
        for o in out["orders"]:
            if not o["symbol"]:
                o["symbol"] = by_id.get(o["order_id"])

        acct = {}
        for v in ib.accountValues():
            if v.tag in ("NetLiquidation", "TotalCashValue", "UnrealizedPnL",
                         "RealizedPnL") and v.currency in ("USD", "BASE"):
                acct.setdefault(v.tag, {})[v.currency] = v.value
        out["account"] = acct
        out["ok"] = True
    except Exception as exc:
        out["error"] = f"query failed: {str(exc)[:200]}"
    finally:
        try:
            ib.disconnect()
        except Exception:
            pass

    print(json.dumps(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
