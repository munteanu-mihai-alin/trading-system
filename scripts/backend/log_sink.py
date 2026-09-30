#!/usr/bin/env python3
"""Plain-HTTP debug log sink for the mobile app.

Listens on 0.0.0.0:8090 and appends POST bodies to logs/app_debug.log.
This is deliberately HTTP, not HTTPS: a guaranteed fallback channel to
collect app-side diagnostics (connection attempts, fetch errors) when
the TLS backend connection is failing and we can't otherwise see why.

Only reachable over WireGuard -- UFW restricts 8090 to 10.66.66.0/24,
same as the main backend. It stores whatever the app posts; treat the
contents as untrusted text.

Run: python3 scripts/backend/log_sink.py   (systemd unit: hft_log_sink)
"""

from __future__ import annotations

import datetime
import pathlib
from http.server import BaseHTTPRequestHandler, HTTPServer

LOG = pathlib.Path(__file__).resolve().parents[2] / "logs" / "app_debug.log"
LOG.parent.mkdir(parents=True, exist_ok=True)
PORT = 8090


class Handler(BaseHTTPRequestHandler):
    def _write(self, body: str) -> None:
        ts = datetime.datetime.now(datetime.timezone.utc).strftime(
            "%Y-%m-%dT%H:%M:%SZ"
        )
        with LOG.open("a", encoding="utf-8") as f:
            f.write(f"[{ts}] {self.client_address[0]} {body}\n")

    def do_POST(self) -> None:  # noqa: N802
        try:
            n = int(self.headers.get("Content-Length", "0") or "0")
        except ValueError:
            n = 0
        body = self.rfile.read(n).decode("utf-8", "replace") if n > 0 else ""
        self._write(body)
        self.send_response(204)
        self.end_headers()

    def do_GET(self) -> None:  # noqa: N802
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.end_headers()
        self.wfile.write(b"log-sink ok\n")

    def log_message(self, *args) -> None:  # silence default stderr spam
        pass


if __name__ == "__main__":
    print(f"log_sink listening on 0.0.0.0:{PORT} -> {LOG}", flush=True)
    HTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
