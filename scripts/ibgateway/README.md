# IB Gateway container (self-built)

Runs Interactive Brokers' IB Gateway + IBC inside a locally built
Docker image so the C++ engine can reach a live/paper IBKR API socket
on `127.0.0.1:4002` (paper) / `4001` (live) on Hetzner — no dev laptop
required.

Nothing here pulls a third-party pre-built image: the container holds
brokerage credentials, so every layer comes from either the official
IBKR installer or the IbcAlpha IBC release, fetched by pinned URL in
[`Dockerfile`](Dockerfile).

## Versions

Proven-stable pairing (the gnzsnz/ib-gateway-docker "stable" combo):
**IB Gateway 10.45.1j** (IBKR "stable" channel) + **IBC 3.24.1**. Both
fetched by pinned URL in the [`Dockerfile`](Dockerfile). IBC's newer
3.24.2 left the JavaFX login fields empty on x64 under the Gateway's
bundled Java 25; 3.24.1 populates them.

## Security model

- **Credentials** live in `/etc/hft/ibkr_user` and
  `/etc/hft/ibkr_password` (mode `600`, root-only), mounted read-only
  as Docker secret files. Never passed as env on the command line
  (which would leak via `docker inspect`).
- **API socket** published only to host loopback (`127.0.0.1`), never
  the public interface.
- Container runs `--cap-drop=ALL --security-opt no-new-privileges`
  with a memory cap.
- **Egress filtering was considered and dropped**: IBKR's server IPs
  rotate, so an allowlist risks intermittently breaking the trading
  connection -- worse than the exfiltration risk it mitigates.

## One-time setup (run on Hetzner as root)

### 1. Create the credential secret files

Do this yourself — do not paste credentials into any script or chat.

```bash
sudo install -m 700 -d /etc/hft
printf '%s' 'YOUR_IBKR_USERNAME' | sudo tee /etc/hft/ibkr_user > /dev/null
printf '%s' 'YOUR_IBKR_PASSWORD' | sudo tee /etc/hft/ibkr_password > /dev/null
sudo chmod 600 /etc/hft/ibkr_user /etc/hft/ibkr_password
```

### 2. Set the trading mode

```bash
echo 'TRADING_MODE=paper' | sudo tee /etc/hft/ibkr.env > /dev/null
sudo chmod 600 /etc/hft/ibkr.env
```

Use `paper` first. Switch to `live` only after a clean paper run.

### 3. Build the image

```bash
cd /mnt/HC_Volume_105581071/trading-system/scripts/ibgateway
sudo docker build -t hft/ibgateway:local .
```

### 4. Install and start the service

```bash
sudo cp ../systemd/hft_ibgateway.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now hft_ibgateway
sudo systemctl status hft_ibgateway
```

### 5. First login — 2FA if prompted

Paper login with live credentials often completes with no 2FA. When
IBKR does require it (typically live, or after a session expires), it
sends a push to the IBKR Mobile app — approve it. With **Seamless
Authentication** enabled in IBKR Account Settings, subsequent
reconnects go ~1 week before prompting again.

## Verify

```bash
# API socket listening on host loopback:
ss -tlnp | grep -E '4001|4002'
# Container logs (login progress, API ready):
sudo docker logs -f hft-ibgateway
```

Then point the engine config at `host=127.0.0.1 paper_port=4002`.

## IP rotation

IBKR data-centre IPs rotate. Re-run `setup_network.sh` (or wire the
companion timer) to refresh the `ibkr_hosts` ipset if the gateway
stops connecting after working previously.
