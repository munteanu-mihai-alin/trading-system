# IB Gateway operations playbook

How we keep the IBKR Gateway (the headless TWS client our `hft_app`
talks to) running 24/7 on Hetzner. This file is a reference for the
operator -- not consumed by code.

Audience: anyone who can SSH to Hetzner. Assumes the Gateway is
already installed under `/opt/ibgateway/` (the offline installer
from IBKR's downloads page).

## What IBKR does to us nightly

- **Auto-restart every day around 23:45 ET** (the "daily server
  reset"). The Gateway disconnects from IBKR's backend, the GUI
  process stays up locally, but the API socket stops accepting
  connections for ~5-10 minutes.
- **Auto-restart every Saturday around 00:00 ET** ("weekly maintenance
  reset"). Same effect, longer downtime (~30 min).
- **Auto-logout once a week** if you DON'T have "Auto restart"
  enabled in the Gateway's Configuration -> Lock and Exit settings.
  Always enable it.

Our engine handles the brief reconnection window via the audit #7
subscription replay + audit #8 error 1100/1101 dispatch (codes 1100
pauses entries, 1101/1102 resumes + replays subs). We don't need to
do anything programmatic during the nightly reset; the engine
handles it.

What we DO need to handle: the **2FA prompt at login**, the
**session staying authenticated past midnight**, and the rare
**actual log-out** (different from the auto-restart).

## Standing config inside the Gateway

Set these once in the Gateway's GUI (via VNC to the Hetzner host
during initial install):

| Setting | Value | Why |
|---|---|---|
| Configuration -> Settings -> API -> Socket port | `4002` (paper) / `4001` (live) | engine's `paper_port` / `live_port` default |
| Configuration -> Settings -> API -> "Enable ActiveX and Socket Clients" | checked | otherwise `eConnect` is refused |
| Configuration -> Settings -> API -> "Allow connections from localhost only" | checked | hft_app runs on the same host |
| Configuration -> Settings -> Lock and Exit -> Auto restart | checked | otherwise weekly logout |
| Configuration -> Settings -> Lock and Exit -> "Lock application after X minutes" | UN-checked | locking blocks the API too |
| Configuration -> Settings -> Lock and Exit -> "Existing Session Detection" -> "Reconnect This Existing Session" | selected | so a re-launch reclaims our slot |

## 2FA at login

IBKR offers three methods. Recommended for headless: **IBKR Mobile
App's "Notification"** push.

1. Install IBKR Mobile (iOS/Android), enable Two-Factor Authentication
   in Account Management.
2. When you start the Gateway (via SSH + a script that launches the
   binary), the Gateway sends a push to your phone.
3. You tap "Approve" on the phone. Gateway logs in. No password
   typing on the server.

The push expires in ~3 minutes; you have to be near your phone when
the engine restart cycle runs. For unattended overnight, the
**Auto-restart** + **Reconnect This Existing Session** combination
keeps the previously-authenticated session alive across the nightly
restart so 2FA is NOT re-prompted.

## IBC (IB Controller) -- the headless / auto path

Once you're past initial-install, the manual GUI approach gets
tedious. IBC (https://github.com/IbcAlpha/IBC) is the community-
maintained "service wrapper" that:

- Auto-clicks the 2FA "Yes" button when the Gateway is restarted by
  systemd (works around the nightly server reset).
- Disables modal dialogs ("Do you want to save log?", etc.).
- Restarts the Gateway on crash.
- Exposes a `ibc-restart` shell command for ops.

Install:
```bash
cd /opt
sudo git clone https://github.com/IbcAlpha/IBC.git
cd IBC
# Configure paths for your Gateway version in config.ini:
#   TWS_PATH=/opt/ibgateway
#   IbDir=/root/ibc/ibc/Jts
#   GatewayOrTws=GATEWAY
#   PaperOrLive=PAPER
#   LoginId=YOUR_USER
#   Password=YOUR_PASS   (kept root-only readable)
sudo bash gatewaystart.sh
```

systemd unit for IBC (NOT in this repo; install on Hetzner):
```ini
[Unit]
Description=IB Gateway via IBC
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/opt/IBC/gatewaystart.sh
Restart=on-failure
RestartSec=30s

[Install]
WantedBy=multi-user.target
```

Then `systemctl enable --now ibgateway.service`. The hft_app systemd
unit's `After=network-online.target` ordering is enough -- IBC
brings the Gateway up before hft_app fires its first `eConnect`.

## Manual reconnect procedure

If you see the engine in `Engine -> Error` state with code 2 ("IBKR
transport connect failed"):

```bash
# 1. Is the Gateway running?
ssh hetzner 'pgrep -fa ibgateway'

# 2. Is anything listening on the port?
ssh hetzner 'ss -ltn | grep -E "4001|4002"'

# 3. If yes, can a fresh client connect?
ssh hetzner 'nc -zv 127.0.0.1 4002'

# 4. If the Gateway is up + port open but the engine fails:
#    look in /opt/ibgateway/jts.ini for any disabled API config,
#    then restart hft_app:
ssh hetzner 'systemctl restart hft_app'

# 5. If the Gateway is DOWN:
ssh hetzner 'systemctl restart ibgateway'   # or gatewaystart.sh
# wait for the 2FA push, tap approve
ssh hetzner 'systemctl restart hft_app'

# 6. If 2FA push doesn't come (rare): VNC into Hetzner via your
#    preferred client, click through the dialog manually.
#    Hetzner Cloud Console -> 'Open Console' gives you a browser
#    VNC session for emergencies.
```

## What to watch in the engine logs

| Symptom | Likely cause | Action |
|---|---|---|
| Engine raises code 354 / 10167 | L1 / L2 data subscription missing in IBKR Account Mgmt | Activate the sub. See agent/AGENT_HANDOFF_LOG.md sub-item 7 for the canonical list. |
| Engine raises code 1100 every night around 23:45 ET | Nightly server reset | Expected. 1101 should arrive within ~10 min and the engine auto-resumes via `reissue_subscriptions`. |
| Engine raises code 1100 and 1101 NEVER comes | Gateway didn't reconnect | Reconnect procedure above. |
| Engine raises code 200 for a specific symbol | `primary_exchange_for(symbol)` override missing | Run `scripts/ibkr_symbol_contract_probe.py`, add the override. |
| Engine raises code 322 (duplicate order id) | next_order_id_ drift | Audit #6's `request_ids(-1)` on connect should self-heal; if it persists, restart hft_app. |
| `notify.sh` keeps firing "hft_app not running" | EXPECT_RUNNING=true outside RTH | Set EXPECT_RUNNING=false in /etc/hft/monitor.env outside trading hours, or rely on hft_app-rth-start.timer to flip it. |

## When all else fails

Stop trading first, ask questions later:

```bash
ssh hetzner 'kill -USR2 $(pgrep -f bin/hft_app)'   # SIGUSR2 = force liquidate
# Then if that doesn't help (engine itself wedged):
ssh hetzner 'systemctl stop hft_app'
ssh hetzner 'systemctl stop hft_app-rth-start.timer hft_app-rth-stop.timer'
# Now nothing will auto-restart; debug at leisure.
```

To resume after fix:
```bash
ssh hetzner 'systemctl start hft_app-rth-start.timer hft_app-rth-stop.timer'
# next RTH open will bring the engine up automatically.
```
