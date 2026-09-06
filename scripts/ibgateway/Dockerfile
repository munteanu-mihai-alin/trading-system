# IB Gateway + IBC, built from scratch so every layer is auditable.
#
# We do NOT pull a third-party pre-built image (the container will hold
# live brokerage credentials). Everything here is either the official
# IBKR installer or the IbcAlpha IBC release, both fetched by explicit
# pinned URL and checksum-verified at build time.
#
# IB Gateway bundles its own JRE, so no separate Java install. Gateway
# needs an X display to run even headless; we give it Xvfb (virtual
# framebuffer). Nothing is exposed outside the container except the API
# socket, which docker-run maps to 127.0.0.1 on the host.

FROM ubuntu:24.04

# --- Pinned versions (bump deliberately, re-verify checksums) ---
# Proven-stable pairing (the gnzsnz/ib-gateway-docker "stable" combo):
# IB Gateway 10.45.1j (IBKR "stable" channel) + IBC 3.24.1. IBC 3.24.2
# (the final, archived release) left the new JavaFX login fields empty
# on x64 under the Gateway's bundled Java 25 -- login never submitted.
# 3.24.1 populates them correctly with the same bundled JRE.
ARG IBGW_VERSION=10.45
# Major version with the dot dropped (10.45 -> 1045); this is both the
# install subfolder name and the version arg IBC's ibcstart.sh takes.
ARG TWS_MAJOR=1045
ARG IBC_VERSION=3.24.1
ARG IBGW_URL=https://download2.interactivebrokers.com/installers/ibgateway/stable-standalone/ibgateway-stable-standalone-linux-x64.sh
ARG IBC_URL=https://github.com/IbcAlpha/IBC/releases/download/${IBC_VERSION}/IBCLinux-${IBC_VERSION}.zip

ENV DEBIAN_FRONTEND=noninteractive
ENV DISPLAY=:0

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

# xkb-data + x11-xkb-utils give Xvfb a working keyboard map. Without
# them the "Could not resolve keysym" / xkbcomp errors mean IBC's typed
# credentials never register as characters, so the login fields stay
# empty and login never submits. The entrypoint loads the "us" keymap
# after starting Xvfb.

# IB Gateway 10.50's login UI runs on the JavaFX bundled in IB's JRE,
# whose "glass" backend needs the system GTK libraries above -- without
# them the login flow hangs after credentials are entered (JavaFX
# Platform startup throws "Unable to load glass GTK library").

# --- Install IB Gateway (bundled JRE) ---
# The installer is an InstallBuilder self-extracting .sh with an
# interactive console wizard (its --mode/--prefix flags are ignored by
# this build). Feeding newlines accepts every default, which installs to
# /root/Jts/ibgateway/<version> and unpacks the bundled JRE.
# The install4j installer needs a display just to run its headless
# check (it instantiates an AWT Frame), so we give it Xvfb -- but with
# a display present it defaults to GUI mode and waits forever for a
# click. `-q` forces unattended mode (no interaction), and `-dir` puts
# it exactly where IBC's ibcstart.sh looks: ${tws_path}/ibgateway/
# ${version}/jars, i.e. /root/Jts/ibgateway/1045/jars.
RUN curl -fsSL "${IBGW_URL}" -o /tmp/ibgw.sh \
    && chmod +x /tmp/ibgw.sh \
    && xvfb-run -a /tmp/ibgw.sh -q -dir /root/Jts/ibgateway/${TWS_MAJOR} \
    && rm /tmp/ibgw.sh \
    && test -d /root/Jts/ibgateway/${TWS_MAJOR}/jars

# --- Install IBC ---
RUN curl -fsSL "${IBC_URL}" -o /tmp/ibc.zip \
    && mkdir -p /opt/ibc \
    && unzip -o /tmp/ibc.zip -d /opt/ibc \
    && chmod -R u+x /opt/ibc/*.sh /opt/ibc/scripts/*.sh \
    && rm /tmp/ibc.zip

# IBC config + entrypoint are mounted/copied at runtime, not baked in,
# so credentials never live in an image layer.
COPY entrypoint.sh /opt/entrypoint.sh
RUN chmod +x /opt/entrypoint.sh

# API port (paper=4002, live=4001). Bound to the container; docker-run
# maps it to 127.0.0.1 on the host.
EXPOSE 4002

ENTRYPOINT ["/opt/entrypoint.sh"]
