# Databento Backtest Report

- Period: `2026-04-13T13:30:00Z` to `2026-05-01T20:00:00Z`
- Symbols: `AAPL, NVDA, AMD`
- L1 dataset/schema: `EQUS.MINI` / `mbp-1`
- L2 dataset/schema: `XNAS.ITCH` / `mbp-10`
- Target profit: `0.8000%`
- Cost per share: `0.000000`
- hftbacktest filled count: `0/3`
- Best-bid target-touch count: `3/3`

| Symbol | Entry | Sell target | L2 steps | Avg spread bps | Target touched | hft filled | Equity | Error |
|---|---:|---:|---:|---:|---|---|---:|---|
| AAPL | 100.0100 | 100.8101 | 200 | 1.98 | yes | no | 0.0000 | LinearAsset |
| NVDA | 100.0100 | 100.8101 | 200 | 1.98 | yes | no | 0.0000 | LinearAsset |
| AMD | 100.0100 | 100.8101 | 200 | 1.98 | yes | no | 0.0000 | LinearAsset |

## hftbacktest Errors

The CSV download/metric path completed, but at least one symbol could not complete the hftbacktest engine run.
