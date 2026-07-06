# Mobile app design

Status: **scaffolded, code lives in the sibling repo `D:/hft-mobile/`**
(kept out of this repo because it has its own Node toolchain, Expo
release cycle, and TypeScript build; the C++ / Python worlds don't
share dependencies with it). React Native + Expo + TypeScript. Five
working screens: Login, Live, Runs, Run detail, Launch. The backend
(`scripts/backend/api.py` in this repo) is the contract.

This document remains the design spec + deferred-work tracker; the
running code is in `../hft-mobile/`.

Origin: backlog items 6, 9, and parts of 12 in the [2026-05-31]
product backlog entry.

## Goal

A phone-first view of the running engine + a launcher for backtests
+ an escalation chat to Claude when something is wrong. So the
operator can:

- Get push notifications when the engine stops, fills a trade, or
  trips a threshold.
- Open the app, see "is it running?" at a glance.
- See today's PnL, fill latencies, open positions.
- Browse historical backtests, view their reports.
- Launch a new backtest with a few taps (preset + period + symbols).
- Ask Claude to investigate when an alert fires.

## Architecture

```
  ┌────────────────────────────────┐
  │ Phone (React Native or Flutter)│
  │   - bearer token in keychain   │
  │   - FCM/APNS push receiver     │
  │   - SwiftUI/Compose-style UI   │
  └──────────────┬─────────────────┘
                 │ HTTPS (wireguard or SSH tunnel)
                 ▼
  ┌────────────────────────────────┐
  │ Hetzner FastAPI                │
  │   scripts/backend/api.py       │
  │   uvicorn :8088 (127.0.0.1)    │
  └──────────────┬─────────────────┘
                 │ reads
                 ▼
  ┌────────────────────────────────┐
  │ hft_app + reports/runs/ +      │
  │ /etc/hft/{api,notify,monitor}  │
  │ systemd journal                │
  └────────────────────────────────┘
```

We do NOT run FastAPI on the public internet. The phone either:

- **Option A (recommended)**: wireguard. Phone has a wireguard
  client (WG iOS / WG Android), tunnel goes to Hetzner, app talks
  to `http://10.66.66.1:8088`. ~5 minutes of one-time setup.
- **Option B**: SSH tunnel. Termius (or similar) forwards
  `localhost:8088` from phone to Hetzner. The app's `BASE_URL` is
  `http://localhost:8088`. Less reliable on flaky cellular.

Push notifications go through:
- `notify.sh` -> ntfy.sh -> the phone's ntfy app. Free, works
  today, no app build needed.
- Once the real app ships, switch to FCM/APNS via a separate hook
  in `notify.sh` that posts to Firebase / Apple's servers.

## Screens

### Home / live status

- Big green/red dot: engine state. Tap for journal tail.
- Cards:
  - Realized PnL today (numbers come from running `compute_metrics`
    over `reports/runs/{latest}/orders.csv` -- the backend can do
    this on-demand).
  - Open positions count + total notional.
  - Last 5 orders (placed / filled).
  - Free disk / mem on Hetzner.

### Runs list

- Paginated list, newest first.
- Each row: label, start time, n_trips, realized PnL, win rate,
  Sharpe.
- Tap a row -> Run detail (below).

Backend endpoint already shipped: `GET /runs`.

### Run detail

- Embedded `report.html` (the file already exists post-`plot_run.py`).
- Below: order log table + open positions table.

Backend endpoint already shipped: `GET /runs/{id}`.

### Launch backtest

- Form:
  - Preset (radio): 10-day / Yen / COVID / custom.
  - Target profit %: slider, default 0.008.
  - Period: two date pickers, defaults from preset.
  - Symbol universe: dropdown of `config/symbols_*.txt` files +
    "default 50-symbol universe".
- Databento remaining credits banner at top (calls
  `GET /databento/credits`).
- Big "Launch" button -> `POST /backtests`.
- After launch -> jumps to a "running" view that polls
  `GET /backtests/{id}` and tails the journal.

Backend endpoint already shipped: `POST /backtests`,
`GET /databento/credits`, `GET /backtests` (list).

### Chat with Claude

- Free-form text field + "Include last 50 log lines" toggle.
- Platform picker (Claude / OpenAI / Cursor).
- Send -> `POST /chat`.

Backend endpoint **stubbed**: `POST /chat` returns 501 today. Next
step is wiring the actual Anthropic / OpenAI / Cursor API calls
with bearer tokens stored in `/etc/hft/llm.env`.

## Choosing a framework

| | React Native | Flutter | SwiftUI + Compose |
|---|---|---|---|
| Cross-platform | ✓ | ✓ | separate codebases |
| Native feel | good | great | native |
| Dev complexity | medium | medium | high (2 codebases) |
| Push notifications | FCM + APNS via libs | same | native |
| Best for | quick iteration | quick iteration | best UX, slowest dev |

Recommended: **React Native + Expo**. We don't need native UI
polish; we need a phone-side dashboard with push and a backtest
launcher. Expo's managed workflow gets us to a TestFlight build in
hours and an Android APK trivially.

## Endpoint contract (frozen for v1)

| Endpoint | Verb | Auth | Response |
|---|---|---|---|
| `/health` | GET | none | `{ok, version}` |
| `/runs` | GET | bearer | `{runs: [{id, n_round_trips_closed, realized_pnl_net, ...}]}` |
| `/runs/{id}` | GET | bearer | `{id, metrics, orders_head, has_report_html}` |
| `/live/status` | GET | bearer | `{process: {running, pid, rss_mb, elapsed}, log_tail: [...]}` |
| `/databento/credits` | GET | bearer | `{available, raw?, reason?, manual_url?}` |
| `/backtests` | GET | bearer | `{units: [{name, load, active, sub}]}` |
| `/backtests` | POST | bearer | `{unit, stdout, stderr, returncode}` |
| `/chat` | POST | bearer | (501 today) |

Auth = `X-HFT-Token: <value-of-API_TOKEN-in-/etc/hft/api.env>`.

## What's blocked

- The chat endpoint needs an LLM API key + a place to put it. Not
  blocking the rest of the app; it just shows "Coming soon" in the
  Chat tab.
- The Databento credits endpoint returns 200 even when the credits
  API itself is unreachable (degrades gracefully). The mobile app
  shows "manual check at databento.com" link in that case.
- Push notifications via FCM/APNS need an Apple developer account
  and a Google Play console. For v1 we use ntfy.sh which works
  today without either.

## Backlog items this design covers (in part or full)

- Item 6 (mobile app) -- design + backend shipped, frontend TBD.
- Item 8 (backtest launcher daemon) -- backend ✓.
- Item 9 (launch test with params) -- backend ✓; frontend TBD.
- Item 12 (improvement ideas: per-symbol decision rate, budget gate
  hit rate, error code histogram, etc.) -- backend extension TBD;
  needs additional engine instrumentation (decisions.csv joins).

## What to ship next on this thread

1. Wire `POST /chat` to Anthropic Messages API. ~50 LOC.
2. Add `GET /metrics/today` -- compute metrics for an *in-flight*
   trading session by running `compute_metrics` over the running
   engine's flat `reports/*.csv` (not yet archived to a run folder).
3. Start the React Native + Expo scaffolding under `mobile/`.
   Hello-world first; the Home screen + token storage second.
4. ntfy.sh subscription topic on the phone (manual setup, 5 min).

Each of those is a fresh `#todo` in `agent/AGENT_HANDOFF_LOG.md`
when picked up.
