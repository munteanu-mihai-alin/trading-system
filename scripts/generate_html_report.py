#!/usr/bin/env python3
"""Generate a self-contained HTML report for a backtest run.

Reads everything inside reports/runs/<run_id>/ and emits a single
report.html with:
  - Run header (label, window, started/ended)
  - Manifest (verbatim JSON)
  - Config (verbatim INI)
  - Metrics table (all of metrics.json)
  - Round-trips table (one row per closed trade, with net PnL + holding)
  - Open positions table (one row per still-open position at end)
  - Equity curve, per-trade PnL, and every per-symbol plot
    (base64-embedded, so the HTML is portable as a single file)
  - Full orders.csv, decisions.csv, l2_trace.csv as data tables
  - step_trace.csv: head + tail sample only (the full file is too large
    to embed without making the HTML > 50 MB)

The "don't exclude anything" principle: every artifact in the run
folder is referenced in the HTML, either embedded directly or with a
visible note explaining where to find it.

Usage:
  python scripts/generate_html_report.py reports/runs/<run_id>
  python scripts/generate_html_report.py reports/runs/<run_id> --l1-dir data/l1

If plots/ or metrics.json don't exist yet, this script runs plot_run.py
first to generate them.
"""

from __future__ import annotations

import argparse
import base64
import configparser
import html
import json
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Optional

import pandas as pd

# We re-use plot_run.py's round-trip derivation so the report tables are
# bit-for-bit consistent with the plots.
SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))
import plot_run  # type: ignore  # noqa: E402

STEP_TRACE_SAMPLE_HEAD = 50
STEP_TRACE_SAMPLE_TAIL = 50


def b64_image(path: Path) -> str:
    data = path.read_bytes()
    return base64.b64encode(data).decode("ascii")


def read_text_safe(path: Path) -> str:
    if not path.exists():
        return ""
    return path.read_text(encoding="utf-8", errors="replace")


def read_json_safe(path: Path) -> Optional[Dict]:
    if not path.exists():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return None


def read_csv_skipping_comments(path: Path) -> pd.DataFrame:
    if not path.exists():
        return pd.DataFrame()
    return pd.read_csv(path, comment="#")


def read_config(run_dir: Path) -> Dict[str, str]:
    cfg = configparser.ConfigParser()
    p = run_dir / "config.ini"
    if not p.exists():
        return {}
    cfg.read(p)
    out: Dict[str, str] = {}
    for section in cfg.sections():
        for key, value in cfg.items(section):
            out[key] = value
    return out


def ensure_plots_and_metrics(run_dir: Path, l1_dir: Path) -> None:
    """If plots/ or metrics.json are missing, run plot_run.py to make them."""
    plots_dir = run_dir / "plots"
    metrics = run_dir / "metrics.json"
    if plots_dir.is_dir() and any(plots_dir.glob("*.png")) and metrics.exists():
        return
    print(f"[html_report] plots/metrics missing; running plot_run.py first",
          file=sys.stderr)
    subprocess.run(
        [sys.executable, str(SCRIPT_DIR / "plot_run.py"), str(run_dir),
         "--l1-dir", str(l1_dir)],
        check=True,
    )


# --------------------------- HTML helpers --------------------------- #


CSS = """
:root {
  color-scheme: light dark;
  --bg: #fdfdfd;
  --fg: #1f1f1f;
  --muted: #5a5a5a;
  --accent: #1f77b4;
  --border: #dcdcdc;
  --code-bg: #f4f4f4;
  --row-alt: #f7f9fb;
}
@media (prefers-color-scheme: dark) {
  :root {
    --bg: #1c1c1c;
    --fg: #e6e6e6;
    --muted: #a0a0a0;
    --accent: #4ea3e0;
    --border: #3a3a3a;
    --code-bg: #2a2a2a;
    --row-alt: #232323;
  }
}
* { box-sizing: border-box; }
body {
  font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto,
               "Helvetica Neue", Arial, sans-serif;
  margin: 0;
  padding: 24px 32px;
  background: var(--bg);
  color: var(--fg);
  max-width: 1200px;
  margin-left: auto;
  margin-right: auto;
  line-height: 1.45;
}
h1 { font-size: 26px; margin: 0 0 4px 0; }
h2 {
  font-size: 19px;
  margin: 32px 0 12px 0;
  padding-bottom: 6px;
  border-bottom: 1px solid var(--border);
}
h3 { font-size: 16px; margin: 24px 0 10px 0; }
.subtle { color: var(--muted); font-size: 13px; }
.meta-grid {
  display: grid;
  grid-template-columns: max-content 1fr;
  gap: 4px 16px;
  margin: 12px 0 0 0;
  font-size: 13px;
}
.meta-grid dt { color: var(--muted); }
.meta-grid dd { margin: 0; font-family: ui-monospace, Menlo, Consolas, monospace; }
pre {
  background: var(--code-bg);
  padding: 12px 16px;
  border-radius: 6px;
  overflow-x: auto;
  font-size: 12px;
  margin: 8px 0;
}
table {
  border-collapse: collapse;
  width: 100%;
  font-size: 13px;
  margin: 12px 0;
}
th, td {
  border: 1px solid var(--border);
  padding: 6px 10px;
  text-align: left;
  vertical-align: top;
}
th {
  background: var(--code-bg);
  font-weight: 600;
}
tbody tr:nth-child(even) td { background: var(--row-alt); }
.metric-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
  gap: 10px;
  margin: 12px 0 4px 0;
}
.metric-card {
  border: 1px solid var(--border);
  border-radius: 6px;
  padding: 10px 14px;
  background: var(--row-alt);
}
.metric-card .label {
  color: var(--muted);
  font-size: 12px;
  text-transform: uppercase;
  letter-spacing: 0.04em;
}
.metric-card .value {
  font-family: ui-monospace, Menlo, Consolas, monospace;
  font-size: 18px;
  font-weight: 600;
  margin-top: 4px;
}
.metric-card.pos .value { color: #2ca02c; }
.metric-card.neg .value { color: #d62728; }
img.plot {
  max-width: 100%;
  height: auto;
  margin: 8px 0 16px 0;
  border: 1px solid var(--border);
  border-radius: 4px;
}
details summary {
  cursor: pointer;
  font-weight: 500;
  padding: 6px 0;
}
.toc {
  background: var(--row-alt);
  border: 1px solid var(--border);
  border-radius: 6px;
  padding: 12px 18px;
  margin: 16px 0 24px 0;
  font-size: 13px;
}
.toc a {
  color: var(--accent);
  text-decoration: none;
  margin-right: 18px;
}
.toc a:hover { text-decoration: underline; }
.note {
  border-left: 3px solid var(--accent);
  padding: 8px 12px;
  background: var(--row-alt);
  font-size: 13px;
  margin: 12px 0;
}
"""


def render_metric_cards(metrics: Dict) -> str:
    if not metrics:
        return "<p class='note'>No metrics.json found.</p>"
    # Highlight a handful of headline metrics first with colored cards.
    headline_keys = [
        "n_round_trips_closed", "n_positions_open_at_end",
        "realized_pnl_net", "win_rate", "sharpe_ratio_annualized",
        "max_drawdown_dollars",
    ]
    cards = []
    for k in headline_keys:
        if k not in metrics:
            continue
        v = metrics[k]
        klass = ""
        if isinstance(v, (int, float)) and k.endswith("_pnl_net"):
            klass = "pos" if v > 0 else ("neg" if v < 0 else "")
        cards.append(
            f"<div class='metric-card {klass}'>"
            f"<div class='label'>{html.escape(k)}</div>"
            f"<div class='value'>{html.escape(str(v))}</div>"
            f"</div>"
        )
    headline_html = (
        f"<div class='metric-grid'>{''.join(cards)}</div>" if cards else ""
    )

    # Full table for completeness.
    rows = "".join(
        f"<tr><th>{html.escape(k)}</th><td>{html.escape(str(v))}</td></tr>"
        for k, v in metrics.items()
    )
    table_html = (
        f"<details><summary>Full metrics table</summary>"
        f"<table>{rows}</table></details>"
    )
    return headline_html + table_html


def render_image(path: Path, title: str) -> str:
    if not path.exists():
        return f"<p class='note'>Plot missing: {html.escape(str(path.name))}</p>"
    b64 = b64_image(path)
    return (
        f"<h3>{html.escape(title)}</h3>"
        f"<img class='plot' alt='{html.escape(title)}' "
        f"src='data:image/png;base64,{b64}' />"
    )


def render_dataframe(df: pd.DataFrame, title: str, max_rows: Optional[int] = None,
                     note: Optional[str] = None) -> str:
    if df.empty:
        return f"<p class='note'>No data: {html.escape(title)}</p>"
    if max_rows is not None and len(df) > max_rows:
        head = df.head(max_rows // 2)
        tail = df.tail(max_rows // 2)
        skipped = len(df) - len(head) - len(tail)
        skipped_row = (
            "<tr><td colspan='"
            f"{len(df.columns)}' style='text-align:center;color:var(--muted);"
            f"font-style:italic'>"
            f"... {skipped} rows omitted ...</td></tr>"
        )
        # Build the two halves as HTML and splice in the spacer row.
        head_html = head.to_html(index=False, border=0, classes="data-table",
                                 escape=True)
        tail_rows = tail.to_html(index=False, border=0, classes="data-table",
                                 escape=True, header=False)
        # Extract just the tbody of tail to avoid duplicate <table> wrappers.
        # Simpler: render both, then surgically splice.
        full_df = pd.concat([head, tail], ignore_index=True)
        # We'll insert the skipped-row marker manually after head_html's tbody.
        # Render full once, then insert.
        full_html = full_df.to_html(index=False, border=0, classes="data-table",
                                    escape=True)
        # Inject the spacer row after the n_head-th body row.
        n_head = len(head)
        body_rows = full_html.split("<tr>")
        # body_rows[0] = preamble + thead; body_rows[1..] = tr fragments
        if len(body_rows) > n_head + 1:
            insert_at = n_head + 1  # +1 because index 0 is preamble
            body_rows.insert(insert_at, skipped_row.lstrip("<tr>"))
            full_html = "<tr>".join(body_rows)
        body = full_html
    else:
        body = df.to_html(index=False, border=0, classes="data-table",
                          escape=True)
    note_html = f"<p class='subtle'>{html.escape(note)}</p>" if note else ""
    return f"{note_html}{body}"


def render_round_trips(trips: List[plot_run.RoundTrip],
                       net_pnl: List[float]) -> str:
    closed = [(i, t, p) for i, (t, p) in enumerate(zip(trips, net_pnl))
              if not t.open_at_end]
    if not closed:
        return "<p class='note'>No closed round-trips.</p>"
    rows = []
    for i, t, p in closed:
        held = (t.holding_market_minutes
                if not pd.isna(t.holding_market_minutes)
                else t.holding_minutes)
        cell_class = "pos" if p > 0 else ("neg" if p < 0 else "")
        rows.append(
            f"<tr>"
            f"<td>{i+1}</td>"
            f"<td>{html.escape(t.symbol)}</td>"
            f"<td>{t.entry_price:.4f}</td>"
            f"<td>{t.exit_price:.4f}</td>"
            f"<td>{t.qty:g}</td>"
            f"<td class='{cell_class}' style='color:"
            f"{'#2ca02c' if p > 0 else ('#d62728' if p < 0 else 'inherit')}'>"
            f"{p:+.4f}</td>"
            f"<td>{held:.1f}</td>"
            f"</tr>"
        )
    return (
        "<table>"
        "<thead><tr><th>#</th><th>Symbol</th><th>Entry</th><th>Exit</th>"
        "<th>Qty</th><th>Net PnL ($)</th><th>Held (market min)</th></tr></thead>"
        f"<tbody>{''.join(rows)}</tbody></table>"
    )


def render_open_positions(trips: List[plot_run.RoundTrip]) -> str:
    opens = [t for t in trips if t.open_at_end]
    if not opens:
        return "<p class='subtle'>None.</p>"
    rows = []
    for t in opens:
        notional = t.entry_price * t.qty
        rows.append(
            f"<tr><td>{html.escape(t.symbol)}</td>"
            f"<td>{t.entry_price:.4f}</td>"
            f"<td>{t.qty:g}</td>"
            f"<td>{notional:.2f}</td></tr>"
        )
    return (
        "<table>"
        "<thead><tr><th>Symbol</th><th>Entry</th><th>Qty</th>"
        "<th>Notional ($)</th></tr></thead>"
        f"<tbody>{''.join(rows)}</tbody></table>"
    )


# ---------------------------- Driver ------------------------------- #


def generate_report(run_dir: Path, l1_dir: Path) -> Path:
    ensure_plots_and_metrics(run_dir, l1_dir)

    manifest = read_json_safe(run_dir / "manifest.json")
    metrics = read_json_safe(run_dir / "metrics.json") or {}
    cfg = read_config(run_dir)

    orders = read_csv_skipping_comments(run_dir / "orders.csv")
    decisions = read_csv_skipping_comments(run_dir / "decisions.csv")
    l2_trace = read_csv_skipping_comments(run_dir / "l2_trace.csv")
    step_trace = read_csv_skipping_comments(run_dir / "step_trace.csv")

    commission_per_share = float(cfg.get("commission_per_share", 0.0035))
    min_per_order = float(
        cfg.get("commission_min_per_order", cfg.get("min_per_order", 0.35))
    )

    trips = plot_run.derive_round_trips(orders, commission_per_share,
                                         min_per_order)
    # Hydrate market timestamps for the holding-time column.
    symbols = sorted({t.symbol for t in trips})
    l1_by_symbol: Dict[str, pd.DataFrame] = {}
    for sym in symbols:
        csv_path = l1_dir / f"{sym}.mbp1.csv"
        if csv_path.exists():
            l1_by_symbol[sym] = pd.read_csv(csv_path)
    plot_run.attach_market_timestamps(trips, l1_by_symbol)
    net_pnl = plot_run.apply_commissions(trips, commission_per_share,
                                          min_per_order)

    # Plot images.
    plots_dir = run_dir / "plots"
    equity = render_image(plots_dir / "equity_curve.png", "Cumulative PnL")
    pnl_bars = render_image(plots_dir / "pnl_per_trade.png", "Per-trade PnL")
    symbol_plots = []
    for sym in symbols:
        symbol_plots.append(render_image(plots_dir / f"{sym}.png",
                                          f"{sym} - price + entry/exit"))

    # Header values from manifest.
    label = cfg.get("run_label", "(no label)")
    started = manifest.get("started_at", "?") if manifest else "?"
    ended = manifest.get("ended_at", "?") if manifest else "?"
    window = f"{cfg.get('databento_start', '?')} -> {cfg.get('databento_end', '?')}"

    # step_trace note: too large to embed fully.
    step_trace_kb = (run_dir / "step_trace.csv").stat().st_size // 1024 \
        if (run_dir / "step_trace.csv").exists() else 0

    # Build the HTML.
    html_doc = f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8" />
<title>Backtest report - {html.escape(label)}</title>
<style>{CSS}</style>
</head>
<body>

<h1>Backtest report</h1>
<dl class="meta-grid">
  <dt>Run label</dt><dd>{html.escape(label)}</dd>
  <dt>Run id</dt><dd>{html.escape(run_dir.name)}</dd>
  <dt>Window</dt><dd>{html.escape(window)}</dd>
  <dt>Started</dt><dd>{html.escape(started)}</dd>
  <dt>Ended</dt><dd>{html.escape(ended)}</dd>
  <dt>Universe size</dt><dd>{html.escape(cfg.get('universe_size', '?'))}</dd>
  <dt>top_k</dt><dd>{html.escape(cfg.get('top_k', '?'))}</dd>
  <dt>max_open_symbols</dt><dd>{html.escape(cfg.get('max_open_symbols', '?'))}</dd>
</dl>

<div class="toc">
  <a href="#metrics">Metrics</a>
  <a href="#round-trips">Round-trips</a>
  <a href="#open">Open positions</a>
  <a href="#equity">Equity curve</a>
  <a href="#per-trade">Per-trade PnL</a>
  <a href="#per-symbol">Per-symbol plots</a>
  <a href="#orders">Orders</a>
  <a href="#decisions">Decisions</a>
  <a href="#l2-trace">L2 trace</a>
  <a href="#step-trace">Step trace</a>
  <a href="#manifest">Manifest</a>
  <a href="#config">Config</a>
</div>

<h2 id="metrics">Headline metrics</h2>
{render_metric_cards(metrics)}

<h2 id="round-trips">Closed round-trips</h2>
{render_round_trips(trips, net_pnl)}

<h2 id="open">Open positions at end</h2>
{render_open_positions(trips)}

<h2 id="equity">Equity curve</h2>
{equity}

<h2 id="per-trade">Per-trade PnL</h2>
{pnl_bars}

<h2 id="per-symbol">Per-symbol plots</h2>
{"".join(symbol_plots) if symbol_plots else "<p class='subtle'>No symbol plots (no closed trades).</p>"}

<h2 id="orders">Orders (full)</h2>
<p class="subtle">{len(orders)} rows from orders.csv.</p>
{render_dataframe(orders, "orders", max_rows=None)}

<h2 id="decisions">Decisions (full)</h2>
<p class="subtle">{len(decisions)} rows from decisions.csv (per-buy ranking snapshots).</p>
{render_dataframe(decisions, "decisions", max_rows=None)}

<h2 id="l2-trace">L2 trace (full)</h2>
<p class="subtle">{len(l2_trace)} rows from l2_trace.csv (per-step L2 snapshot when a sell is being scored).</p>
{render_dataframe(l2_trace, "l2_trace", max_rows=None)}

<h2 id="step-trace">Step trace (sample)</h2>
<p class="subtle">{len(step_trace)} rows, {step_trace_kb} KB. Showing first {STEP_TRACE_SAMPLE_HEAD} and last {STEP_TRACE_SAMPLE_TAIL} rows; the full file is at step_trace.csv next to this report.</p>
{render_dataframe(step_trace, "step_trace", max_rows=STEP_TRACE_SAMPLE_HEAD + STEP_TRACE_SAMPLE_TAIL, note="Sampled - full file embedded would push this HTML above 50 MB.")}

<h2 id="manifest">Manifest</h2>
<pre>{html.escape(json.dumps(manifest, indent=2, sort_keys=True) if manifest else "(no manifest.json)")}</pre>

<h2 id="config">Config</h2>
<pre>{html.escape(read_text_safe(run_dir / "config.ini") or "(no config.ini)")}</pre>

<p class="subtle" style="margin-top: 40px; text-align: center;">
Generated by scripts/generate_html_report.py - all plots embedded as base64; this file is self-contained.
</p>

</body>
</html>
"""

    out = run_dir / "report.html"
    out.write_text(html_doc, encoding="utf-8")
    return out


def main(argv: List[str]) -> int:
    p = argparse.ArgumentParser()
    p.add_argument("run_dir", type=Path)
    p.add_argument("--l1-dir", type=Path, default=Path("data/l1"))
    args = p.parse_args(argv)

    if not args.run_dir.is_dir():
        print(f"html_report: not a directory: {args.run_dir}", file=sys.stderr)
        return 2

    out = generate_report(args.run_dir, args.l1_dir)
    size_kb = out.stat().st_size // 1024
    print(f"html_report: wrote {out} ({size_kb} KB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
