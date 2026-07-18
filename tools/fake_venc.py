#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# §9.6 actuation-harness endpoint: a stand-in venc HTTP API that records
# every request with a monotonic timestamp to a JSONL log and answers
# 200 {"ok":true}. No state, no validation — the harness checker owns the
# assertions (tools/actuation_udp_bench.sh).
#
#   fake_venc.py <port> <log.jsonl>
import json
import os
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def main() -> None:
    port, log_path = int(sys.argv[1]), sys.argv[2]
    log = open(log_path, "a", buffering=1, encoding="utf-8")
    fail_first = int(os.environ.get("FAIL_FIRST", "0"))
    attempts = 0

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_GET(self):  # noqa: N802 (http.server API)
            nonlocal attempts
            is_probe = self.path == "/ping"
            if not is_probe:
                attempts += 1
            log.write(json.dumps({"t": time.monotonic(),
                                  "path": self.path}) + "\n")
            try:
                body = b'{"ok":true}'
                fail = not is_probe and attempts <= fail_first
                self.send_response(503 if fail else 200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            except (ConnectionResetError, BrokenPipeError):
                pass  # the actuator may close after the status line

        def log_message(self, *args):
            pass

    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()


if __name__ == "__main__":
    main()
