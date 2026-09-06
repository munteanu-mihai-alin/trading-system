# Runtime image for the HFT engine binary, published to ghcr.io by CI
# (.github/workflows/ci.yml, "Publish hft_app image" step). The build
# context holds the freshly-built hft_app plus the shared libraries it
# links against (copied from dependencies/linux/install/lib), so the
# image is self-contained and never hits the "libprotobuf.so.X not
# found" failure a bare binary on the host does (see the 2026-09-07
# paper-launch debugging: the stale bin/hft_app could not load
# libprotobuf.so.29.3.0).
#
# Run it against a local IB Gateway with host networking, mounting the
# repo dir for config.ini / data / reports / logs:
#
#   docker run --rm --network host -v "$PWD":/work \
#     ghcr.io/munteanu-mihai-alin/trading-system/hft_app:main
#
# The engine reads config.ini from the working dir; [broker] mode picks
# ibkr_paper (4002) / live (4001). Python-strategy variants (chronos2
# forecasts) additionally need the venv mounted -- a follow-up.
FROM ubuntu:24.04

# Base runtime libs that are not part of the vendored bundle. Everything
# else the binary needs is copied into /opt/hft/lib below.
RUN apt-get update && apt-get install -y --no-install-recommends \
        libstdc++6 \
        zlib1g \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY hft_app /opt/hft/hft_app
COPY lib/ /opt/hft/lib/
ENV LD_LIBRARY_PATH=/opt/hft/lib

# Report any unresolved shared library at build time (non-fatal so a
# publish is never silently blocked; the log makes a genuine miss
# obvious).
RUN chmod +x /opt/hft/hft_app \
    && ( ldd /opt/hft/hft_app | grep -i "not found" \
         && echo "WARNING: unresolved libraries above" \
         || echo "all shared libraries resolved" )

WORKDIR /work
ENTRYPOINT ["/opt/hft/hft_app"]
