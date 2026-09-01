# HawkesOU-MR: OU and Hawkes Logic Explained

Reference doc for the `HawkesOU_MR*.py` family in `research/quantconnect/`.
Explains the two signal components with formal math notation, concrete
per-bar calculations, and a full worked order example.

Theory framework: Cartea, A., Jaimungal, S., Penalva, J. — *Algorithmic
and High-Frequency Trading* (Cambridge, 2015).

---

## 1. Overview

Two-component ranked entry system:

- **Hawkes intensity** — a per-symbol self-exciting counter that reflects
  recent price activity. Used as the **ranking score**.
- **Ornstein-Uhlenbeck (OU) trailing mean** — a per-symbol EWMA of mid.
  Used as the **entry gate**: only buy when price is at-or-below its own
  trailing mean.

Each bar: update both estimators for every symbol; rank by Hawkes; top-K
candidates check the OU gate + budget; passers get a market buy with a
limit sell attached at `entry × (1 + target)`.

---

## 2. Ornstein-Uhlenbeck component

### 2.1 Continuous-time OU process (classical)

The underlying stochastic model:

$$
dX_t = \theta \, (\mu - X_t) \, dt + \sigma \, dW_t
$$

- $X_t$ : observed process (here, the symbol's mid price)
- $\mu$ : long-run mean
- $\theta$ : mean-reversion speed (larger $\theta$ = snappier reversion)
- $\sigma$ : instantaneous volatility
- $W_t$ : standard Brownian motion

Steady-state distribution:

$$
X_\infty \sim \mathcal{N}\!\left(\mu, \; \frac{\sigma^{2}}{2\theta}\right)
$$

### 2.2 Discrete estimator used in the code

The code doesn't estimate $\theta$ and $\sigma$ separately — it only needs
the running mean $\hat{\mu}_t$ for the entry gate. It uses a
$\Delta t$-weighted exponential moving average with wall-clock half-life
$h$ = `OU_HALFLIFE_SECONDS`:

$$
\tau \;=\; \frac{h}{\ln 2}
$$

$$
\alpha_t \;=\; 1 \;-\; e^{-\Delta t / \tau}
$$

$$
\hat{\mu}_t \;=\; (1 - \alpha_t) \, \hat{\mu}_{t-1} \;+\; \alpha_t \, m_t
$$

Where:

- $m_t$ : mid price at bar $t$
- $\Delta t$ : wall-clock seconds since the last update
- $\hat{\mu}_t$ : running estimator of the OU mean $\mu$

Note this is the discrete Bayesian-mean update for OU when the innovation
noise is $\mathcal{N}(0, \sigma^2)$ and observations arrive at irregular
$\Delta t$ — the EWMA weight depends on elapsed time, not bar count.

### 2.3 Entry gate

A buy candidate qualifies iff:

$$
m_t \;\leq\; \hat{\mu}_t \,\bigl(1 + \varepsilon_{\text{OU}}\bigr)
$$

Where $\varepsilon_{\text{OU}}$ = `OU_BUY_THRESHOLD_PCT`:

- **`HawkesOU-MR`** (base): $\varepsilon_{\text{OU}} = 0$ → strict; mid must be at or below the trailing mean.
- **`HawkesOU-MR-WideGate-Top5`**: $\varepsilon_{\text{OU}} = 3 \times 10^{-3}$ → allow up to 30 bps above.
- **`HawkesOU-MR-NoGate`**: gate removed entirely; every top-K candidate passes.

### 2.4 Numerical example — NVDA, 6 Minute bars

Constants: $h = 1800\text{ s}$, so $\tau = 1800 / \ln 2 \approx 2597\text{ s}$.
Bar cadence $\Delta t = 60\text{ s}$, so
$\alpha = 1 - e^{-60/2597} \approx 0.0228$.

| Bar $k$ | $m_{t_k}$ (\$) | update | $\hat{\mu}_{t_k}$ (\$) |
|---|---|---|---|
| 1 | 140.00 | prime | 140.000 |
| 2 | 140.15 | $0.9772 \cdot 140.000 + 0.0228 \cdot 140.15$ | 140.003 |
| 3 | 140.30 | $0.9772 \cdot 140.003 + 0.0228 \cdot 140.30$ | 140.010 |
| 4 | 140.05 | $0.9772 \cdot 140.010 + 0.0228 \cdot 140.05$ | 140.011 |
| 5 | 139.80 | $0.9772 \cdot 140.011 + 0.0228 \cdot 139.80$ | 140.006 |
| 6 | 139.50 | $0.9772 \cdot 140.006 + 0.0228 \cdot 139.50$ | 139.994 |

At bar 6: $m_{t_6} = 139.50 < \hat{\mu}_{t_6} = 139.994$ → OU gate passes
with $\varepsilon_{\text{OU}} = 0$.

---

## 3. Hawkes component

### 3.1 Continuous-time Hawkes intensity

Self-exciting point process; intensity jumps on events and decays
exponentially between them:

$$
\lambda(t) \;=\; \mu \;+\; \int_{0}^{t} \alpha \, e^{-\beta (t - s)} \, dN_s
$$

- $\lambda(t)$ : instantaneous intensity (events per unit time)
- $\mu$ : baseline intensity
- $\alpha$ : jump size on each event
- $\beta$ : decay rate (memory $\propto 1/\beta$)
- $N_s$ : counting process (increments by 1 at each event time)

Stationarity requires $\alpha < \beta$; long-run expected intensity is
$\mathbb{E}[\lambda] = \frac{\mu \beta}{\beta - \alpha}$. With the code's
$\mu=10, \alpha=5, \beta=20$: $\mathbb{E}[\lambda] = \frac{200}{15} \approx 13.3$
in a genuinely self-exciting regime.

### 3.2 Discrete-time update used in code

Between successive updates at times $t_{k-1}, t_k$ with
$\Delta t = t_k - t_{k-1}$:

$$
\lambda_k \;=\; \mu \;+\; (\lambda_{k-1} - \mu) \, e^{-\beta \Delta t} \;+\; \alpha \cdot \mathbb{1}_{\{E_k\}}
$$

Where $\mathbb{1}_{\{E_k\}}$ is 1 if an event fired at bar $k$, else 0.
Parameters as constants:

$$
\mu = 10, \quad \alpha = 5, \quad \beta = 20
$$

### 3.3 Event trigger (mid-move proxy)

QC's equity feed doesn't emit trade events, so the code proxies "activity"
via price moves. An event fires when the mid has moved at least
$\theta_{\text{bps}}$ basis points from the last-firing mid:

$$
E_k \;\iff\; \bigl| m_{t_k} - m^{\star} \bigr| \cdot \frac{10^4}{m^{\star}} \;\geq\; \theta_{\text{bps}}
$$

where $m^{\star}$ is the mid at the last event, and
$\theta_{\text{bps}}$ = `HAWKES_MID_CHANGE_THRESHOLD_BPS` $= 2.5$.
On firing, $m^{\star}$ is updated to the current mid.

### 3.4 Calibration caveat at Minute cadence

The decay term $e^{-\beta \Delta t}$ is what makes a Hawkes process actually
*remember* recent events. The code's $\beta = 20$ was chosen for the C++
engine's tick cadence (~15 ms):

$$
\text{C++ tick } \Delta t = 0.015\text{ s} \;\Rightarrow\; \beta \Delta t = 0.3 \;\Rightarrow\; e^{-\beta \Delta t} \approx 0.74
$$

The intensity retains ~74% of its previous excitation per tick — genuine
Hawkes behaviour.

In QC with Minute bars:

$$
\text{QC Minute } \Delta t = 60\text{ s} \;\Rightarrow\; \beta \Delta t = 1200 \;\Rightarrow\; e^{-\beta \Delta t} \approx e^{-1200} \approx 0
$$

The decay term vanishes on every update. Effective intensity collapses to:

$$
\lambda_k \;\approx\; \mu + \alpha \cdot \mathbb{1}_{\{E_k\}} \;=\;
\begin{cases}
\mu + \alpha = 15 & \text{if event this bar} \\
\mu = 10 & \text{otherwise}
\end{cases}
$$

So the Hawkes ranking is **binary at Minute cadence**: 15 vs 10.

To restore genuine memory with a half-life $h_{\lambda}$ of, say, one bar
($h_{\lambda} = 60\text{ s}$), retune:

$$
\beta \;=\; \frac{\ln 2}{h_{\lambda}} \;=\; \frac{\ln 2}{60} \;\approx\; 0.01155
$$

That would give $e^{-\beta \cdot 60} = e^{-0.693} = 0.5$ per bar — halving
the excitation every 60 s, comparable to the OU half-life.

### 3.5 Numerical example — NVDA, 5 bars (as-coded, β=20)

| Bar $k$ | $m_{t_k}$ | $\Delta$ bps vs $m^{\star}$ | event $E_k$ | $\Delta t$ (s) | $\lambda_k$ |
|---|---|---|---|---|---|
| 1 | 140.00 | prime | — | — | 10.0 |
| 2 | 140.15 | 10.7 (≥ 2.5) | 1 | 60 | $10 + (10-10) \cdot e^{-1200} + 5 = 15$ |
| 3 | 140.16 | 0.7 vs 140.15 | 0 | — | 15.0 (no update) |
| 4 | 140.30 | 10.7 vs 140.15 | 1 | 120 | $10 + (15-10) \cdot e^{-2400} + 5 \approx 15$ |
| 5 | 140.31 | 0.7 vs 140.30 | 0 | — | 15.0 (no update) |

Notice how in bar 4 the "memory" term $(15-10) \cdot e^{-2400}$ is
numerically indistinguishable from zero — the previous intensity is
completely forgotten. Post-event $\lambda$ always resets to $\mu + \alpha$.

---

## 4. Assembly — ranking, gate, order routing

Per bar, define for each symbol $i \in \{1, \ldots, N\}$:

$$
s_i \;=\; \lambda_i
$$

(the score is the raw Hawkes intensity; no tilt in the current code)

Sort symbols by $s_i$ descending. Let $\pi$ be the resulting permutation
so $s_{\pi(1)} \geq s_{\pi(2)} \geq \ldots$

Top-$K$ candidate set:

$$
\mathcal{C} \;=\; \{\pi(1), \pi(2), \ldots, \pi(K)\}
$$

Where $K$ is dynamic under the reinvest schedule (see §6).

For each $i \in \mathcal{C}$ in rank order:

1. **Skip if already exposed**:
   $\text{skip if } (\text{held}_i \lor \text{pending\_buy}_i)$
2. **OU gate**:
   $\text{skip if } m_i > \hat{\mu}_i (1 + \varepsilon_{\text{OU}})$
3. **Budget check**:
   $\text{break if } C_t + N_{\text{trade}} > B_t$
   (where $C_t$ = currently committed notional, $B_t$ = current budget)
4. **Size**:
   $q_i = \lfloor N_{\text{trade}} / m_i \rfloor$;
   $\text{skip if } q_i = 0$
5. **Place order**:
   `market_order(symbol_i, q_i)`
6. **Update committed**:
   $C_t \leftarrow C_t + q_i \cdot m_i$

Where $N_{\text{trade}}$ = `TRADE_NOTIONAL` = 500.

---

## 5. Exit

On buy-fill event at price $p_{\text{fill}}$ with quantity $q$:

$$
p_{\text{target}} \;=\; p_{\text{fill}} \cdot (1 + \varepsilon_{\text{profit}})
$$

$$
\text{place: } \verb|limit_order(symbol, -q, |p_{\text{target}}\verb|)|
$$

Where $\varepsilon_{\text{profit}}$ = `TARGET_PROFIT_PCT` = 0.025 (2.5%).

**Never sells at a loss.** If $p_{\text{target}}$ is never touched, the
lot sits open indefinitely. This is a definitional artifact producing
100% win rate in backtest metrics — see the fundability discussion in
`AGENT_HANDOFF_LOG.md`.

---

## 6. Reinvest schedule (dynamic caps)

The strategy compounds realized wins into more concurrent slots. Let
$R_t$ = `self.portfolio.total_profit` (realized only, ignoring
unrealized).

Slots gained so far:

$$
g_t \;=\; \left\lfloor \frac{\max(0, R_t)}{I} \right\rfloor
$$

Dynamic caps:

$$
K_t \;=\; K_0 + g_t
\qquad\qquad
B_t \;=\; B_0 + g_t \cdot I
$$

Where:
- $K_0$ = `INITIAL_TOP_K` = 3 (or 5 for the Wide variant)
- $B_0$ = `INITIAL_BUDGET` = 1500
- $I$ = `BUDGET_INCREMENT` = 500 (equal to `TRADE_NOTIONAL`)

$K_t$ and $B_t$ are monotonically non-decreasing — drawdowns cannot
shrink them. Uses only realized profit so a stalled underwater lot
cannot inflate the deployment.

---

## 7. Full worked example — one bar's decisions

Assume the base variant with $K_0 = 3$, $B_0 = 1500$, $\varepsilon_{\text{OU}} = 0$,
$\varepsilon_{\text{profit}} = 0.025$, and $g_t = 0$ (no reinvest yet).

State across 5 symbols after 30 min of trading:

| Symbol | $m_i$ | $\lambda_i$ | $\hat{\mu}_i$ | $m_i \leq \hat{\mu}_i$? |
|---|---|---|---|---|
| NVDA | 140.20 | 15 | 140.00 | ❌ |
| AMD | 150.00 | 15 | 151.20 | ✅ |
| INTC | 30.50 | 15 | 30.80 | ✅ |
| MU | 100.00 | 15 | 102.00 | ✅ |
| AAPL | 220.00 | 10 | 219.00 | ❌ |

**Rank** by $s_i = \lambda_i$: NVDA, AMD, INTC, MU (all tied at 15 —
tie-broken by insertion order), then AAPL (10).

**Top-3**: $\mathcal{C} = \{\text{NVDA}, \text{AMD}, \text{INTC}\}$.

**Iterate** entries, tracking committed $C$:

- **NVDA**: not held. OU gate: $m = 140.20 > \hat{\mu}(1+0) = 140.00$ → **skip**.
- **AMD**: not held. Gate passes. Budget: $C = 0$, $C + 500 = 500 \leq 1500$ ✓.
  Size: $q = \lfloor 500 / 150 \rfloor = 3$. Notional: $3 \cdot 150 = 450$.
  **Emit** `market_order("AMD", 3)`. Update $C \leftarrow 450$.
- **INTC**: not held. Gate passes. Budget: $C + 500 = 950 \leq 1500$ ✓.
  Size: $q = \lfloor 500 / 30.50 \rfloor = 16$. Notional: $16 \cdot 30.50 = 488$.
  **Emit** `market_order("INTC", 16)`. Update $C \leftarrow 938$.

Loop ends. MU was inside top-K by score AND passed the OU gate AND had
budget room, but was iterated *after* INTC and — being rank-4 — never
reaches the candidate list at $K = 3$.

**On fill (next bar's `on_order_event`):**

- AMD fill at \$150.05 → set `entry_price = 150.05`;
  $p_{\text{target}} = 150.05 \cdot 1.025 = 153.80$;
  place `limit_order("AMD", -3, 153.80)`.
- INTC fill at \$30.52 → set `entry_price = 30.52`;
  $p_{\text{target}} = 30.52 \cdot 1.025 = 31.28$;
  place `limit_order("INTC", -16, 31.28)`.

**Later — AMD bid reaches \$153.80** (limit fills):

$$
\text{gross P\&L} \;=\; q \cdot (p_{\text{target}} - p_{\text{fill}}) \;=\; 3 \cdot 3.75 \;=\; \$11.25
$$

$$
\text{fees} \;=\; 2 \cdot \$1.00 \;=\; \$2.00 \quad\text{(IB \$1 min per order, binding at low share counts)}
$$

$$
\text{net} \;=\; \$11.25 - \$2.00 \;=\; \$9.25
$$

Lot free again → available for re-entry on the next bar. Cumulative
realized $R_t$ is closer to unlocking a 4th slot when it crosses \$500.

---

## 8. Observations and next-step suggestions

### 8.1 What drives the P&L in this framework

- **The OU gate is the real discipline.** Without it (Open variant), the
  strategy still works because the mean-reversion effect is embedded in
  the 2.5% target — buying names that recently moved will often revert.
  But the gate materially concentrates entries onto over-sold names.
- **The Hawkes ranking is a very thin signal at Minute cadence.** Per §3.4,
  it degenerates to binary. Most of the "ranking" is effectively
  arbitrary tie-breaking within the events-fired subset.
- **The +2.5% target does the heavy lifting on P&L** — small ratio of gross
  ($ \sim \$3-\$12 $ per fill) to fees ($\$2$/RT), but comfortably positive
  on liquid names.

### 8.2 Fixing the Hawkes to actually contribute signal

Two independent paths:

- **Retune $\beta$ to Minute cadence.** Set $\beta \approx 0.01155$ so
  $e^{-\beta \cdot 60} \approx 0.5$ (one-bar half-life). Multi-event
  clustering in the last few minutes then produces genuinely elevated
  intensity that decays over 5-10 bars.
- **Switch resolution.** `Resolution.SECOND` gives $\Delta t = 1$ so
  $\beta \Delta t = 20$, still $\approx e^{-20} = 2 \times 10^{-9}$ (still fully
  decayed). Sub-second resolution isn't available for equities on the
  standard QC tier, so retuning $\beta$ is the practical fix.

### 8.3 Other levers (from broader microstructure literature)

- **Volatility scaling**: size positions $\propto 1/\hat{\sigma}_i$ to
  equalize risk contribution.
- **Two-sided trading**: mirror the OU gate on the short side —
  $m_i \geq \hat{\mu}_i (1 + \varepsilon)$ triggers a short.
- **Pairs trading**: OU on the *spread* between two related tickers
  instead of raw price. Beta-neutral, typically much cleaner Sharpe.
- **Two-channel Hawkes (buy vs sell aggressors)**: filter out entries
  where the sell-aggressor channel dominates (adverse-selection filter).
  The C++ engine has this; the QC port doesn't yet.

---
