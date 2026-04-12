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


## Additive-only change policy

A guard script is included at:

```bash
scripts/enforce_additive_only.py
```

Usage:

```bash
python3 scripts/enforce_additive_only.py --base /path/to/old --candidate /path/to/new
```

It fails if:
- any file present in the base project is missing from the candidate project
- any text line from the base project was removed in the candidate project

This enforces "only add, do not remove" updates.

## IBKR SDK wiring

The project builds without the IBKR SDK by default.

To enable the real SDK:

```bash
cmake -DHFT_ENABLE_IBKR=ON -DHFT_IBKR_SDK_DIR=/path/to/IBKR ..
```

Expected SDK layout:

```text
<IBKR>/source/cppclient/client/
```

When enabled:
- `IBKRClient` compiles against the official IBKR C++ API
- live mode uses `IBKRClient`
- paper/sim mode uses `PaperBrokerSim`


## Coverage diagnostics

For CI runners where `lcov` omits branch counters, use `scripts/run_coverage_ci.sh`.
It enables both `branch_coverage=1` and `lcov_branch_coverage=1`, checks for `BRF/BRH`, and only then enforces the branch threshold.
