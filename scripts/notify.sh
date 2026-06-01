#!/usr/bin/env bash
# Operator notifier. Called by:
#   - hft-notify@.service (when hft_app fails to start / crashes)
#   - hft_monitor.py     (when mem / disk / RSS crosses a threshold)
#   - SIGUSR1 / SIGUSR2 wrappers (manual kill switches, in case the
#     engine's own warnings don't reach the right channel)
#
# Two backends, configured by env file at /etc/hft/notify.env:
#   TG_BOT_TOKEN=...   # Telegram bot token from @BotFather
#   TG_CHAT_ID=...     # target chat (your user id or a group id)
#   NTFY_TOPIC=...     # ntfy.sh topic for phone push notifications
#                      # (e.g. https://ntfy.sh/$NTFY_TOPIC)
#
# If TG_BOT_TOKEN is set we send via Telegram. If NTFY_TOPIC is set
# we ALSO push via ntfy.sh (good for redundancy). Both are optional;
# with neither set the script logs to stderr and exits 0 so systemd
# doesn't keep retrying the OnFailure unit.
#
# Usage:
#   scripts/notify.sh "any message text"
#   scripts/notify.sh "hft_app down" "level=error"   # second arg = tag
#
# Exit codes:
#   0  notification dispatched (or no-op when nothing configured)
#   1  at least one backend failed AND no other backend succeeded

set -euo pipefail

MSG="${1:-hft notification}"
TAG="${2:-info}"
HOSTNAME=$(hostname -s 2>/dev/null || echo "?")

# Source the env file if present. Errors are non-fatal: a missing
# notify.env file means notification is disabled, not broken.
if [[ -f /etc/hft/notify.env ]]; then
  # shellcheck disable=SC1091
  set -a; . /etc/hft/notify.env; set +a
fi

PAYLOAD="[${HOSTNAME}/${TAG}] ${MSG}"
ANY_SENT=0
ANY_FAILED=0

if [[ -n "${TG_BOT_TOKEN:-}" && -n "${TG_CHAT_ID:-}" ]]; then
  # Telegram bot API. Plain text + parse_mode disabled so we don't
  # have to escape user content.
  if curl -fsSL --max-time 10 \
      "https://api.telegram.org/bot${TG_BOT_TOKEN}/sendMessage" \
      -d "chat_id=${TG_CHAT_ID}" \
      --data-urlencode "text=${PAYLOAD}" \
      >/dev/null; then
    ANY_SENT=1
  else
    ANY_FAILED=1
    echo "notify.sh: Telegram send failed" >&2
  fi
fi

if [[ -n "${NTFY_TOPIC:-}" ]]; then
  # ntfy.sh push notification. Set priority via the tag; "error" gets
  # the loudest sound on the phone, anything else is normal.
  PRI="default"
  [[ "$TAG" == "error" || "$TAG" == "crash" ]] && PRI="urgent"
  if curl -fsSL --max-time 10 \
      -H "Title: hft_app" \
      -H "Priority: $PRI" \
      -H "Tags: $TAG" \
      -d "$PAYLOAD" \
      "https://ntfy.sh/${NTFY_TOPIC}" \
      >/dev/null; then
    ANY_SENT=1
  else
    ANY_FAILED=1
    echo "notify.sh: ntfy push failed" >&2
  fi
fi

if [[ "$ANY_SENT" == 0 ]]; then
  if [[ "$ANY_FAILED" == 1 ]]; then
    echo "notify.sh: all backends failed for: $PAYLOAD" >&2
    exit 1
  fi
  # Nothing configured. Log to stderr so journald captures it, exit 0
  # so systemd doesn't think the OnFailure hook itself failed.
  echo "notify.sh (no backend configured): $PAYLOAD" >&2
fi
