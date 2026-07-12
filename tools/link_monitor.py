#!/usr/bin/env python3
"""waybeam-link fleet monitor — UDP NDJSON stats -> browser dashboard.

Each waybeam-link instance (tx/rx/loopback) is configured with a stats UDP
egress (PROTOCOL.md §15.3):

    "stats": { "hz": 5, "bind": { "kind": "udp", "send": "<monitor-ip>:9110" } }

Every instance can send to the SAME monitor port; snapshots are keyed by
(source-ip, node, session) so a whole fleet lands on one dashboard. This
bridge is a pure stdlib translator — it does NOT touch the binaries. It:

  * listens for NDJSON stats datagrams (one §15.3 line per datagram),
  * keeps the latest snapshot per instance (+ liveness age),
  * serves the dashboard at  /            (link_monitor.html, same origin),
  * exposes                  /api/instances (JSON: all current snapshots),
  * pushes                   /api/stream    (SSE: every snapshot as it lands).

No third-party packages. Run:

    python3 tools/link_monitor.py            # HTTP :8099, UDP :9110
    python3 tools/link_monitor.py --http 8099 --udp 9110,9111
    python3 tools/link_monitor.py --label 192.168.2.201=vehicle --label 192.168.2.242=ground

Then open http://localhost:8099/ in a browser.
"""
import argparse
import json
import os
import queue
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
HTML_PATH = os.path.join(HERE, "link_monitor.html")


class Fleet:
    """Thread-safe registry of the latest snapshot per instance."""

    def __init__(self, labels):
        self._lock = threading.Lock()
        self._instances = {}          # key -> record dict
        self._subs = []               # list[queue.Queue] SSE subscribers
        self._labels = labels         # ip -> friendly name

    @staticmethod
    def _key(src_ip, snap):
        return "%s|%s|%s" % (src_ip, snap.get("node"), snap.get("session"))

    def ingest(self, src_ip, raw):
        try:
            snap = json.loads(raw)
        except (ValueError, TypeError):
            return
        if not isinstance(snap, dict) or "t_ms" not in snap:
            return
        key = self._key(src_ip, snap)
        now = time.time()
        rec = {
            "key": key,
            "src": src_ip,
            "label": self._labels.get(src_ip, ""),
            "role": _infer_role(snap),
            "recv_wall": now,
            "snap": snap,
        }
        with self._lock:
            prev = self._instances.get(key)
            rec["first_seen"] = prev["first_seen"] if prev else now
            rec["updates"] = (prev["updates"] + 1) if prev else 1
            self._instances[key] = rec
            dead = []
            for q in self._subs:
                try:
                    q.put_nowait(rec)
                except queue.Full:
                    dead.append(q)
            for q in dead:
                self._subs.remove(q)

    def snapshot_all(self):
        with self._lock:
            return list(self._instances.values())

    def subscribe(self):
        q = queue.Queue(maxsize=256)
        with self._lock:
            self._subs.append(q)
            backlog = list(self._instances.values())
        return q, backlog

    def unsubscribe(self, q):
        with self._lock:
            if q in self._subs:
                self._subs.remove(q)


def _infer_role(snap):
    """No role field in §15.3; infer from what moved."""
    adapters = snap.get("adapters") or []
    tx = any((a.get("tx_submitted") or 0) > 0 or (a.get("tx_reports") or 0) > 0
             for a in adapters)
    rx = any((a.get("rx") or 0) > 0 for a in adapters)
    if tx and not rx:
        return "tx"
    if rx and not tx:
        return "rx"
    if tx and rx:
        return "loopback"
    return "?"


def udp_reader(fleet, bind_addr, port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((bind_addr, port))
    sys.stderr.write("[monitor] listening for NDJSON on udp %s:%d\n"
                     % (bind_addr, port))
    while True:
        try:
            data, addr = sock.recvfrom(65535)
        except OSError:
            break
        # One datagram may carry one or several '\n'-terminated lines.
        for line in data.split(b"\n"):
            line = line.strip()
            if line:
                fleet.ingest(addr[0], line.decode("utf-8", "replace"))


def make_handler(fleet):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *a):  # quiet
            pass

        def _send(self, code, body, ctype):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            path = self.path.split("?", 1)[0]
            if path == "/" or path == "/index.html":
                return self._serve_html()
            if path == "/api/instances":
                return self._serve_instances()
            if path == "/api/stream":
                return self._serve_stream()
            self._send(404, b"not found\n", "text/plain")

        def _serve_html(self):
            try:
                with open(HTML_PATH, "rb") as f:
                    body = f.read()
            except OSError:
                body = b"<h1>link_monitor.html missing beside link_monitor.py</h1>"
            self._send(200, body, "text/html; charset=utf-8")

        def _serve_instances(self):
            now = time.time()
            out = []
            for rec in fleet.snapshot_all():
                out.append({
                    "key": rec["key"], "src": rec["src"], "label": rec["label"],
                    "role": rec["role"], "age_ms": int((now - rec["recv_wall"]) * 1000),
                    "updates": rec["updates"], "snap": rec["snap"],
                })
            body = json.dumps({"now_ms": int(now * 1000), "instances": out}).encode()
            self._send(200, body, "application/json")

        def _serve_stream(self):
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "keep-alive")
            self.end_headers()
            q, backlog = fleet.subscribe()
            try:
                for rec in backlog:
                    self._sse(rec)
                while True:
                    try:
                        rec = q.get(timeout=15)
                        self._sse(rec)
                    except queue.Empty:
                        self.wfile.write(b": keepalive\n\n")
                        self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass
            finally:
                fleet.unsubscribe(q)

        def _sse(self, rec):
            payload = json.dumps({
                "key": rec["key"], "src": rec["src"], "label": rec["label"],
                "role": rec["role"], "recv_ms": int(rec["recv_wall"] * 1000),
                "updates": rec["updates"], "snap": rec["snap"],
            })
            self.wfile.write(b"data: " + payload.encode() + b"\n\n")
            self.wfile.flush()

    return Handler


def main():
    ap = argparse.ArgumentParser(description="waybeam-link fleet monitor")
    ap.add_argument("--http", type=int, default=8099, help="dashboard HTTP port")
    ap.add_argument("--http-bind", default="0.0.0.0", help="dashboard bind addr")
    ap.add_argument("--udp", default="9110",
                    help="comma-separated NDJSON listen ports (default 9110)")
    ap.add_argument("--udp-bind", default="0.0.0.0", help="UDP bind addr")
    ap.add_argument("--label", action="append", default=[],
                    metavar="IP=NAME", help="friendly name for a source IP")
    args = ap.parse_args()

    labels = {}
    for spec in args.label:
        if "=" in spec:
            ip, name = spec.split("=", 1)
            labels[ip.strip()] = name.strip()

    fleet = Fleet(labels)
    for p in args.udp.split(","):
        p = p.strip()
        if p:
            t = threading.Thread(target=udp_reader,
                                 args=(fleet, args.udp_bind, int(p)), daemon=True)
            t.start()

    httpd = ThreadingHTTPServer((args.http_bind, args.http), make_handler(fleet))
    sys.stderr.write("[monitor] dashboard at http://%s:%d/\n"
                     % ("localhost" if args.http_bind == "0.0.0.0" else args.http_bind,
                        args.http))
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        sys.stderr.write("\n[monitor] bye\n")


if __name__ == "__main__":
    main()
