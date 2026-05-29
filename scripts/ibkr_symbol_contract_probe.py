#!/usr/bin/env python3
"""Resolve every symbol in the universe against IBKR via reqContractDetails.

Why this exists
---------------
RealIBKRTransport builds every Contract with `secType="STK"`,
`exchange="SMART"`, `currency="USD"` and no `primaryExchange`. SMART
routing usually picks the right listing, but for dual-listed or
ambiguous names (PSTG was the first one we hit during the L1 backfill)
it either returns multiple matches or fails outright. The engine then
silently never streams L1/L2 for that symbol.

This probe iterates the 50-symbol universe (or any --symbols list),
calls `reqContractDetails` against an IB Gateway, and prints a markdown
table summarising which symbols need an explicit `primaryExchange`
override. Paste the table into `agent/ibkr_symbol_audit.md` and add the
"needs override" entries to
`primary_exchange_override_table()` in `src/lib/SymbolUniverse.cpp`.

Audit context: agent/ibkr_client_audit.md issues #1 and #9.

Pre-reqs:
  - IB Gateway running (paper port 4002 by default)
  - `ibapi` installed in the active venv

Usage:
    python scripts/ibkr_symbol_contract_probe.py
    python scripts/ibkr_symbol_contract_probe.py --symbols config/symbols_yen.txt
    python scripts/ibkr_symbol_contract_probe.py --port 7497 --client-id 99
"""

from __future__ import annotations

import argparse
import sys
import threading
import time
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List

try:
    from ibapi.client import EClient
    from ibapi.contract import Contract, ContractDetails
    from ibapi.wrapper import EWrapper
except ImportError as exc:  # pragma: no cover
    raise SystemExit(
        "ibapi is required: pip install ibapi  (or use the IBKR-provided wheel)"
    ) from exc


# Mirror of include/models/symbol_universe.hpp kSymbolCompanyList.
# Kept in sync by hand; if you add a symbol there, add it here. (We can't
# import the C++ list and IBKR universe membership changes slowly.)
DEFAULT_UNIVERSE = [
    "AAPL", "NVDA", "AMD", "INTC", "MU", "QCOM", "ARM", "ASML", "AMAT",
    "LRCX", "KLAC", "SNPS", "CDNS", "MKSI", "ENTG", "STX", "WDC", "PSTG",
    "DELL", "HPQ", "SMCI", "CSCO", "HPE", "IBM", "KEYS", "TSM", "GFS",
    "UMC", "TSEM", "ASX", "AMKR", "IMOS", "LEA", "AWK", "CEG", "VST",
    "NIO", "XPEV", "OKLO", "SNDK", "LMT", "HWM", "RTX", "NOC", "GSM",
    "DD", "LIN", "APD", "TTE", "NOK",
]


@dataclass
class SymbolResult:
    """Aggregated reqContractDetails outcome for one symbol."""
    symbol: str
    matches: List[ContractDetails] = field(default_factory=list)
    error_code: int = 0
    error_message: str = ""
    finished: bool = False  # contractDetailsEnd fired


class ProbeApp(EWrapper, EClient):
    """One EClient session that drives reqContractDetails across N symbols.

    Each request gets a fresh req_id; results are keyed by req_id so
    parallel-in-flight requests don't trample each other. The per-symbol
    aggregator lives in `results_by_req_id`.

    We serialise requests anyway (one at a time, ~50ms apart) because
    reqContractDetails is cheap on IBKR's side and serial keeps the error
    handling simple.
    """

    def __init__(self):
        EClient.__init__(self, self)
        # Maps req_id -> SymbolResult so per-symbol state can grow as
        # contractDetails callbacks fire.
        self.results_by_req_id: Dict[int, SymbolResult] = {}
        self.next_req_id = 1
        # Signalled when contractDetailsEnd fires for the current request.
        self.current_done = threading.Event()
        # Connection ready flag, flipped by nextValidId callback.
        self.connected_event = threading.Event()

    # ------------------------------------------------------------------
    # EWrapper callbacks
    # ------------------------------------------------------------------

    def nextValidId(self, orderId: int) -> None:
        super().nextValidId(orderId)
        # The TWS API uses the first nextValidId callback as the "connection
        # really is ready" signal; before this fires, reqContractDetails
        # silently no-ops.
        self.connected_event.set()

    def contractDetails(self, reqId: int, contractDetails: ContractDetails) -> None:
        r = self.results_by_req_id.get(reqId)
        if r is not None:
            r.matches.append(contractDetails)

    def contractDetailsEnd(self, reqId: int) -> None:
        r = self.results_by_req_id.get(reqId)
        if r is not None:
            r.finished = True
        self.current_done.set()

    def error(self, reqId, errorTime, errorCode, errorString,
              advancedOrderRejectJson=""):  # noqa: D401
        # Connection-status updates (2104, 2106, 2158, etc.) arrive with
        # reqId=-1 and aren't actually errors; surface only the per-request
        # ones.
        if reqId <= 0:
            return
        r = self.results_by_req_id.get(reqId)
        if r is not None:
            r.error_code = errorCode
            r.error_message = errorString
            # Some error codes (e.g. 200 "no security definition") never
            # produce a contractDetailsEnd. Treat the error as terminal.
            r.finished = True
            self.current_done.set()

    # ------------------------------------------------------------------
    # Driver
    # ------------------------------------------------------------------

    def probe(self, symbol: str, timeout_s: float = 8.0) -> SymbolResult:
        req_id = self.next_req_id
        self.next_req_id += 1

        contract = Contract()
        contract.symbol = symbol
        contract.secType = "STK"
        contract.exchange = "SMART"
        contract.currency = "USD"

        result = SymbolResult(symbol=symbol)
        self.results_by_req_id[req_id] = result

        self.current_done.clear()
        self.reqContractDetails(req_id, contract)
        ok = self.current_done.wait(timeout=timeout_s)
        if not ok:
            result.error_code = -1
            result.error_message = f"timeout after {timeout_s:.0f}s"
            result.finished = False
        return result


def load_symbols(symbols_arg: str | None) -> List[str]:
    if not symbols_arg:
        return list(DEFAULT_UNIVERSE)
    path = Path(symbols_arg)
    if not path.is_file():
        raise SystemExit(f"--symbols file not found: {path}")
    out: List[str] = []
    for line in path.read_text().splitlines():
        t = line.strip()
        if not t or t.startswith("#"):
            continue
        # Allow `SYMBOL,Company` lines just like the C++ loader.
        sym = t.split(",", 1)[0].strip()
        if sym:
            out.append(sym)
    return out


def classify(result: SymbolResult) -> str:
    """Returns a short tag explaining what the report row's action should be."""
    if not result.finished:
        return "TIMEOUT"
    if result.error_code != 0:
        # 200 "No security definition has been found for the request" is
        # the canonical case for "needs an override or is wrong symbol".
        return "ERROR"
    n = len(result.matches)
    if n == 0:
        return "NO_MATCH"
    if n == 1:
        return "OK"
    # IBKR returned multiple contracts under SMART; SMART will pick one
    # but we don't know which, and the chosen one may not be the listing
    # we want. Recommend pinning primaryExchange to the listing reported
    # by the FIRST contract (usually correct, but the operator should
    # verify before adding to the override table).
    return "AMBIGUOUS"


def fmt_row(result: SymbolResult) -> str:
    tag = classify(result)
    if tag == "OK":
        c = result.matches[0].contract
        return (f"| {result.symbol} | OK | {c.primaryExchange or '-'} | "
                f"{c.exchange} | {c.currency} | (single match) |")
    if tag == "AMBIGUOUS":
        listings = ",".join(
            sorted({m.contract.primaryExchange or "-"
                    for m in result.matches})
        )
        return (f"| {result.symbol} | AMBIGUOUS | {listings} | SMART | USD | "
                f"{len(result.matches)} contracts; pin primary_exchange |")
    if tag == "ERROR":
        return (f"| {result.symbol} | ERROR({result.error_code}) | - | "
                f"SMART | USD | {result.error_message} |")
    if tag == "NO_MATCH":
        return f"| {result.symbol} | NO_MATCH | - | SMART | USD | drop or use different secType |"
    return f"| {result.symbol} | {tag} | - | - | - | - |"


def main(argv: List[str]) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=4002,
                   help="IB Gateway port (4002=paper, 7497=TWS paper, "
                        "4001=live, 7496=TWS live)")
    p.add_argument("--client-id", type=int, default=88,
                   help="Pick something different from the trader's "
                        "(default 1) so the probe can run alongside.")
    p.add_argument("--symbols",
                   help="Path to a symbols file (one symbol per line, "
                        "optional ',company'). Defaults to the 50-symbol "
                        "universe baked into this script.")
    p.add_argument("--timeout", type=float, default=8.0,
                   help="Per-symbol timeout in seconds.")
    p.add_argument("--sleep", type=float, default=0.05,
                   help="Pause between symbol requests (seconds).")
    args = p.parse_args(argv)

    symbols = load_symbols(args.symbols)
    print(f"# Probing {len(symbols)} symbols on {args.host}:{args.port} "
          f"(client_id={args.client_id})\n", file=sys.stderr)

    app = ProbeApp()
    app.connect(args.host, args.port, clientId=args.client_id)

    # Run the EReader-equivalent loop in a background thread so callbacks
    # actually fire while we issue requests on the main thread.
    reader = threading.Thread(target=app.run, daemon=True)
    reader.start()

    if not app.connected_event.wait(timeout=10.0):
        print("ERROR: did not receive nextValidId from IB Gateway within 10s; "
              "is the gateway running and reachable?", file=sys.stderr)
        app.disconnect()
        return 2

    rows = []
    counts: Dict[str, int] = defaultdict(int)
    for sym in symbols:
        result = app.probe(sym, timeout_s=args.timeout)
        rows.append(result)
        counts[classify(result)] += 1
        # Live progress to stderr so we can watch progress without
        # polluting the markdown report on stdout.
        print(f"  {sym:6s} -> {classify(result):10s} "
              f"matches={len(result.matches)} "
              f"err={result.error_code or '-'}", file=sys.stderr)
        time.sleep(args.sleep)

    app.disconnect()

    # Emit the report on stdout (clean markdown for copy-paste).
    print("# IBKR Symbol-Contract Probe Report\n")
    print(f"Total symbols: {len(symbols)}")
    for k in ("OK", "AMBIGUOUS", "ERROR", "NO_MATCH", "TIMEOUT"):
        if counts.get(k):
            print(f"  - {k}: {counts[k]}")
    print()
    print("| Symbol | Status | primaryExchange (suggested) | exchange "
          "| currency | Notes |")
    print("|---|---|---|---|---|---|")
    for r in rows:
        print(fmt_row(r))

    print()
    print("Action items:")
    print("  - For each AMBIGUOUS or ERROR row, add an entry to ")
    print("    `primary_exchange_override_table()` in ")
    print("    `src/lib/SymbolUniverse.cpp` using the listing exchange ")
    print("    shown above (verify against the IBKR contract first ")
    print("    via TWS's contract description if unsure).")
    print("  - For NO_MATCH rows, the symbol may need a different ")
    print("    secType (e.g. preferred share class) or should be ")
    print("    removed from the universe.")
    print()

    # Non-zero exit if anything needs attention so CI / shell scripts ")
    # can act on it.
    bad = sum(counts.get(k, 0) for k in ("AMBIGUOUS", "ERROR", "NO_MATCH",
                                         "TIMEOUT"))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
