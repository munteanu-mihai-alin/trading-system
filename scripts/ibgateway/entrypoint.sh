#!/bin/bash
# Container entrypoint: start a virtual X display, generate the IBC
# config from environment (credentials come in as a Docker secret file,
# read into env by the wrapper), then launch IB Gateway under IBC.
#
# Credentials are read from files, never passed as plain env on the
# docker run line (which would leak via `docker inspect`). The systemd
# unit points TWS_USERID_FILE / TWS_PASSWORD_FILE at /run/secrets/*.

set -euo pipefail

# --- Resolve credentials from secret files ---
: "${TWS_USERID_FILE:=/run/secrets/ibkr_user}"
: "${TWS_PASSWORD_FILE:=/run/secrets/ibkr_password}"

if [[ ! -r "${TWS_USERID_FILE}" || ! -r "${TWS_PASSWORD_FILE}" ]]; then
    echo "FATAL: credential secret files not readable:" >&2
    echo "  ${TWS_USERID_FILE}" >&2
    echo "  ${TWS_PASSWORD_FILE}" >&2
    exit 1
fi

TWS_USERID="$(cat "${TWS_USERID_FILE}")"
TWS_PASSWORD="$(cat "${TWS_PASSWORD_FILE}")"

# --- Trading mode: paper (default) or live ---
: "${TRADING_MODE:=paper}"
if [[ "${TRADING_MODE}" == "live" ]]; then
    API_PORT=4001
else
    API_PORT=4002
fi

# --- Generate IBC config.ini from template ---
# NOTE the setting names: IBC reads IbLoginId / IbPassword (NOT
# LoginId / Password). With the wrong names IBC finds the login fields,
# sets EMPTY strings into them (still logging "Setting user name"), and
# the login never submits -- exactly the empty-field hang we chased.
# Settings below mirror the proven gnzsnz/ib-gateway-docker config.
IBC_INI=/opt/ibc/config.ini
cat > "${IBC_INI}" <<EOF
# Generated at container start -- do not edit; edit entrypoint.sh.
IbLoginId=${TWS_USERID}
IbPassword=${TWS_PASSWORD}
TradingMode=${TRADING_MODE}
FIX=no
ReadOnlyLogin=no

# Accept the API connection without a manual dialog click.
AcceptIncomingConnectionAction=accept
AllowBlindTrading=yes

# Give the login dialog time to appear, and don't bail if 2FA takes a
# while (first login needs the phone approval).
LoginDialogDisplayTimeout=60
SecondFactorAuthenticationTimeout=180
ExitAfterSecondFactorAuthenticationTimeout=no
ReloginAfterSecondFactorAuthenticationTimeout=yes

# Auto-restart daily instead of forcing a full re-login. IBKR still
# requires periodic 2FA; with Seamless Authentication enabled on the
# account this reconnects for ~1 week before prompting the phone again.
IbAutoClosedown=no
ClosedownAt=

# Read-only API off -- we place orders.
ReadOnlyApi=no

# Bind the API on all container interfaces; docker maps to host loopback.
OverrideTwsApiPort=${API_PORT}

# Dismiss non-critical dialogs automatically.
AcceptNonBrokerageAccountWarning=yes
DismissPasswordExpiryWarning=yes
DismissNSEComplianceNotice=yes
ExistingSessionDetectedAction=primary
EOF

chmod 600 "${IBC_INI}"

# --- Start virtual display ---
# Mirror the proven gnzsnz Xvfb line exactly: depth 16, -ac (disable
# access control). setxkbmap loads a real keymap (harmless if unused).
Xvfb :0 -ac -screen 0 1024x768x16 &
XVFB_PID=$!
sleep 2
setxkbmap -display :0 us 2>/dev/null || true

# --- Launch IB Gateway under IBC ---
# The standalone installer put Gateway at /root/Jts/ibgateway/<version>,
# so TWS_PATH is /root/Jts and the version arg is the major version
# (e.g. 1045). ibcstart.sh looks for ${TWS_PATH}/ibgateway/<version>.
TWS_MAJOR_VRSN="${TWS_MAJOR_VRSN:-1045}"
export IBC_PATH=/opt/ibc
export LOG_PATH=/opt/ibc/logs
mkdir -p "${LOG_PATH}"

echo "Starting IB Gateway (mode=${TRADING_MODE}, api_port=${API_PORT}, vrsn=${TWS_MAJOR_VRSN})..."
exec /opt/ibc/scripts/ibcstart.sh "${TWS_MAJOR_VRSN}" -g \
    "--tws-path=/root/Jts" \
    "--ibc-path=${IBC_PATH}" \
    "--ibc-ini=${IBC_INI}" \
    "--mode=${TRADING_MODE}"
