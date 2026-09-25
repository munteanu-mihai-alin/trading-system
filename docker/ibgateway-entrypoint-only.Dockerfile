# Minimal derivation: swap ONLY the entrypoint onto the proven image.
# Used to change one thing at a time -- a from-scratch build of
# ibgateway.Dockerfile currently installs Gateway 10.50 rather than the
# 10.45 in hft/ibgateway:local, because IBKR replaces the
# stable-standalone installer in place. Keep both: this for changing
# settings safely, the full Dockerfile for rebuilding from nothing.
FROM hft/ibgateway:local
COPY ibgateway-entrypoint.sh /opt/entrypoint.sh
RUN chmod +x /opt/entrypoint.sh
