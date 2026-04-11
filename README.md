# HFT Merged Recovered

This project consolidates the previously generated code into a single modular codebase.

## Main components

- `include/models/`:
  Hawkes, OU, microstructure helpers, stock state, trade stats
- `include/execution/`:
  latency model, transaction cost model, slippage model, fill model, execution score
- `include/sim/`:
  FIFO order book, queue tracking, simulator
- `include/validation/`:
  calibration bins, KS statistic, rolling error, degradation alarm
- `include/engine/`:
  ranking engine interface and implementation
- `include/bench/`:
  RDTSC utilities and latency summary
- `tests/`:
  unit, math, simulator, and validation tests

## Build

```bash
mkdir build
cd build
cmake ..
make -j
ctest --output-on-failure
./hft_app
```

## Notes

This is a research platform, not a production trading system.
