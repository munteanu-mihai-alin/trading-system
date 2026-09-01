#!/usr/bin/env python3
"""chronos2_forecast.py -- one-shot Chronos-2 forecast bridge for the
C++ Chronos2ExecutionEngine.

Reads a per-symbol daily-close history CSV, runs Chronos-2 for each
symbol (batched where possible), computes auxiliary features (realized
vol, short-term momentum), and writes a predictions CSV that the C++
side parses back into Stock fields.

Same one-shot pattern the C++ engine already uses for
databento_download_l2.py etc. -- spawn, wait, read output file, exit.
Model weights are downloaded from HuggingFace on first run and cached
in ~/.cache/huggingface/.

CLI:
    chronos2_forecast.py \
        --history-csv <path>      # input: symbol,date,close
        --output <path>           # output: symbol,predicted_price,predicted_q25,
                                  #         realized_vol,momentum_5d
        [--model amazon/chronos-2]
        [--context-len 64]
        [--prediction-len 1]
        [--vol-lookback 20]
        [--momentum-lookback 5]
        [--quantile-q25 0.25]

Exit codes:
    0 -- success (output CSV written even if some symbols failed)
    1 -- fatal: model load failed
    2 -- fatal: input CSV missing or malformed
"""

import argparse
import csv
import math
import sys
from collections import defaultdict


def _parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--history-csv", required=True,
                   help="Input CSV: columns symbol,date,close")
    p.add_argument("--output", required=True,
                   help="Output CSV: symbol,predicted_price,predicted_q25,"
                        "realized_vol,momentum_5d")
    p.add_argument("--model", default="amazon/chronos-2")
    p.add_argument("--context-len", type=int, default=64)
    p.add_argument("--prediction-len", type=int, default=1)
    p.add_argument("--vol-lookback", type=int, default=20)
    p.add_argument("--momentum-lookback", type=int, default=5)
    return p.parse_args()


def _load_history(path):
    """Return dict: symbol -> list of floats (daily closes, chronological)."""
    hist = defaultdict(list)
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        required = {"symbol", "date", "close"}
        if not required.issubset(reader.fieldnames or []):
            print(f"[chronos2_forecast] input CSV missing columns "
                  f"{required - set(reader.fieldnames or [])}",
                  file=sys.stderr)
            sys.exit(2)
        # Rows already sorted by date per symbol per the C++ writer;
        # sort defensively anyway.
        rows = list(reader)
    rows.sort(key=lambda r: (r["symbol"], r["date"]))
    for row in rows:
        try:
            hist[row["symbol"]].append(float(row["close"]))
        except (TypeError, ValueError):
            continue
    return hist


def _compute_aux(closes, vol_lookback, momentum_lookback):
    """(realized_vol_annual, momentum_ret) from a list of daily closes."""
    import numpy as np
    vol = 0.0
    mom = 0.0
    if len(closes) >= 2:
        window = np.asarray(closes[-vol_lookback - 1:], dtype=np.float64)
        if len(window) >= 2 and np.all(window > 0):
            log_rets = np.diff(np.log(window))
            if len(log_rets) >= 1:
                std = log_rets.std(ddof=1) if len(log_rets) > 1 else 0.0
                vol = float(std * math.sqrt(252.0))
    if len(closes) >= momentum_lookback + 1:
        past = closes[-momentum_lookback - 1]
        if past > 0:
            mom = (closes[-1] - past) / past
    return vol, mom


def main():
    args = _parse_args()

    hist = _load_history(args.history_csv)
    if not hist:
        print("[chronos2_forecast] input CSV had 0 usable rows",
              file=sys.stderr)
        sys.exit(2)

    # Fail fast if the ML stack isn't installed; error message flows to
    # the C++ engine's stderr, which the engine can log.
    try:
        import torch
        from chronos import BaseChronosPipeline
    except Exception as e:
        print(f"[chronos2_forecast] chronos-forecasting / torch not "
              f"available: {e}", file=sys.stderr)
        sys.exit(1)

    try:
        pipeline = BaseChronosPipeline.from_pretrained(
            args.model,
            device_map="cpu",
            torch_dtype=torch.float32,
        )
    except Exception as e:
        print(f"[chronos2_forecast] failed to load {args.model}: {e}",
              file=sys.stderr)
        sys.exit(1)

    predictions = []
    for symbol in sorted(hist.keys()):
        closes = hist[symbol]
        if len(closes) < args.context_len:
            predictions.append({
                "symbol": symbol,
                "predicted_price": 0.0,
                "predicted_q25": 0.0,
                "realized_vol": 0.0,
                "momentum_5d": 0.0,
            })
            continue

        vol, mom = _compute_aux(closes, args.vol_lookback,
                                args.momentum_lookback)
        try:
            context = torch.tensor(closes[-args.context_len:],
                                   dtype=torch.float32)
            quantiles, mean = pipeline.predict_quantiles(
                context,
                prediction_length=args.prediction_len,
                quantile_levels=[0.1, 0.25, 0.5, 0.75, 0.9],
            )
            pred_mean = float(mean[0, 0].item())
            # Index 1 of quantile_levels is 0.25.
            pred_q25 = float(quantiles[0, 0, 1].item())
        except Exception as e:
            print(f"[chronos2_forecast] {symbol}: predict failed: {e}",
                  file=sys.stderr)
            pred_mean = 0.0
            pred_q25 = 0.0

        predictions.append({
            "symbol": symbol,
            "predicted_price": pred_mean,
            "predicted_q25": pred_q25,
            "realized_vol": vol,
            "momentum_5d": mom,
        })

    with open(args.output, "w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=["symbol", "predicted_price", "predicted_q25",
                        "realized_vol", "momentum_5d"],
        )
        writer.writeheader()
        for row in predictions:
            writer.writerow(row)

    print(f"[chronos2_forecast] wrote {len(predictions)} rows -> "
          f"{args.output}", file=sys.stderr)


if __name__ == "__main__":
    main()
