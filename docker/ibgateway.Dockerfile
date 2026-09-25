# IB Gateway under IBC, headless on Xvfb.
#
# Reconstructed on 2026-09-26 from `docker history` of the hft/ibgateway:local
# image that was built by hand on 2026-09-06 and never committed. Until now
# the container the whole trading system depends on existed only as a layer
# cache on one box: if it were lost there was no way to rebuild it.
#
# Build:
#   docker build -f docker/ibgateway.Dockerfile -t hft/ibgateway:local docker/
#
# REPRODUCIBILITY CAVEAT (confirmed empirically 2026-09-26: a rebuild on
# this date installed Gateway 10.50, not the 10.45 in the original image).
# IBGW_URL points at IBKR's "stable-standalone"
# channel, which is a MOVING target -- IBKR replaces it in place. A rebuild
# today may install a newer Gateway than the 10.45 this was pinned to, while
# TWS_MAJOR stays 1045. The installer writes into the directory it is given
# regardless of its own version, so the build will still succeed and
# ibcstart.sh will still find the path; the label is what goes stale, not the
# build. IBKR publishes no durable per-version URL for the standalone
# installer, so pinning properly means hosting a copy ourselves.
#
# Runtime contract (see scripts/systemd/hft_ibgateway.service):
#   -v /etc/hft/ibkr_user:/run/secrets/ibkr_user:ro
#   -v /etc/hft/ibkr_password:/run/secrets/ibkr_password:ro
#   -e TRADING_MODE=paper|live      (paper -> API 4002, live -> 4001)
#   --network host                  (engine connects on 127.0.0.1)

FROM ubuntu:24.04

ARG IBGW_VERSION=10.45
ARG TWS_MAJOR=1045
ARG IBC_VERSION=3.24.1
ARG IBGW_URL=https://download2.interactivebrokers.com/installers/ibgateway/stable-standalone/ibgateway-stable-standalone-linux-x64.sh
ARG IBC_URL=https://github.com/IbcAlpha/IBC/releases/download/3.24.1/IBCLinux-3.24.1.zip

ENV DEBIAN_FRONTEND=noninteractive
ENV DISPLAY=:0

# Xvfb + the X/GTK libraries the Gateway's Swing UI needs even headless.
# socat and procps are kept for in-container debugging.
RUN apt-get update && apt-get install -y --no-install-recommends \
        xvfb \
        x11-utils \
        x11-xkb-utils \
        xkb-data \
        libxkbcommon0 \
        libxkbcommon-x11-0 \
        libxtst6 \
        libxrender1 \
        libxi6 \
        libxslt1.1 \
        libgtk-3-0t64 \
        libgbm1 \
        libasound2t64 \
        fontconfig \
        fonts-dejavu-core \
        unzip \
        curl \
        ca-certificates \
        socat \
        procps \
    && rm -rf /var/lib/apt/lists/*

# The installer is itself a GUI app, so it needs a display even in -q mode.
# The jars check makes a partial install fail the build rather than produce
# an image that only breaks at first login.
RUN curl -fsSL "${IBGW_URL}" -o /tmp/ibgw.sh \
    && chmod +x /tmp/ibgw.sh \
    && xvfb-run -a /tmp/ibgw.sh -q -dir /root/Jts/ibgateway/${TWS_MAJOR} \
    && rm /tmp/ibgw.sh \
    && test -d /root/Jts/ibgateway/${TWS_MAJOR}/jars

RUN curl -fsSL "${IBC_URL}" -o /tmp/ibc.zip \
    && mkdir -p /opt/ibc \
    && unzip -o /tmp/ibc.zip -d /opt/ibc \
    && chmod -R u+x /opt/ibc/*.sh /opt/ibc/scripts/*.sh \
    && rm /tmp/ibc.zip

# IBC's config.ini is generated at container start, not baked in -- it
# carries the account credentials.
COPY ibgateway-entrypoint.sh /opt/entrypoint.sh
RUN chmod +x /opt/entrypoint.sh

EXPOSE 4002
ENTRYPOINT ["/opt/entrypoint.sh"]
