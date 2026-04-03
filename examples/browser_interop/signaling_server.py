#!/usr/bin/env python3
"""
nanortc browser_interop — HTTP signaling relay server

Two modes:
  - Legacy 2-peer mode: two peers (0, 1) with binary relay (backward compat)
  - Host mode: peer 0 is a persistent host, peers 1..N are viewers

API:
    POST /join              → {"id": N}           Register as a peer (legacy: max 2)
    POST /join?role=host    → {"id": 0}           Register as host (enables multi-viewer)
    POST /send?id=N         body=JSON             Relay message (legacy: to other peer)
    POST /send?id=0&to=M    body=JSON             Host sends to specific viewer
    GET  /recv?id=N         → JSON or 204         Long-poll (query: timeout=5)
    POST /leave?id=N        → {"ok": true}        Unregister peer
    GET  /                  → index.html           Browser UI

Message format (all transports):
    {"type":"offer",     "sdp":"v=0\\r\\n..."}
    {"type":"answer",    "sdp":"v=0\\r\\n..."}
    {"type":"candidate", "candidate":"candidate:..."}

In host mode, messages from viewers to host include "from": N.

Usage:
    python3 signaling_server.py [--port 8765]

SPDX-License-Identifier: MIT
"""

import argparse
import json
import os
import threading
import time
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs

# Peer state: peers[id] = {"msgs": [], "event": Event}
peers = {}
lock = threading.Lock()
host_mode = False
MAX_PEERS = 8


def alloc_peer(role=None):
    global host_mode
    with lock:
        if role == "host":
            host_mode = True
            if 0 not in peers:
                peers[0] = {"msgs": [], "event": threading.Event()}
                return 0
            return -1  # host already registered
        if host_mode:
            # Multi-viewer: assign pid 1..N
            for pid in range(1, MAX_PEERS):
                if pid not in peers:
                    peers[pid] = {"msgs": [], "event": threading.Event()}
                    return pid
            return -1  # full
        # Legacy 2-peer mode
        for pid in (0, 1):
            if pid not in peers:
                peers[pid] = {"msgs": [], "event": threading.Event()}
                return pid
    return -1


def remove_peer(pid):
    global host_mode
    with lock:
        peers.pop(pid, None)
        if pid == 0 and host_mode:
            host_mode = False
            print("[sig] Host left, reverting to legacy mode")
        if not peers:
            print("[sig] All peers gone, ready for new session")


def enqueue(to_id, message):
    with lock:
        if to_id in peers:
            peers[to_id]["msgs"].append(message)
            peers[to_id]["event"].set()
            return True
    return False


def dequeue(pid, timeout):
    """Block until a message is available or timeout expires."""
    deadline = time.monotonic() + timeout
    while True:
        with lock:
            if pid not in peers:
                return None
            if peers[pid]["msgs"]:
                msg = peers[pid]["msgs"].pop(0)
                if not peers[pid]["msgs"]:
                    peers[pid]["event"].clear()
                return msg
            evt = peers[pid]["event"]
            evt.clear()
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return None
        evt.wait(timeout=min(remaining, 1.0))


class SigHandler(BaseHTTPRequestHandler):
    """Handle signaling HTTP requests."""

    def log_message(self, fmt, *args):
        # Suppress default access log, use our own
        pass

    def _send_json(self, obj, status=200):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_html(self, path):
        try:
            with open(path, "rb") as f:
                data = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        except FileNotFoundError:
            self.send_error(404)

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def do_GET(self):
        parsed = urlparse(self.path)
        qs = parse_qs(parsed.query)

        if parsed.path == "/":
            html = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "index.html")
            self._send_html(html)

        elif parsed.path == "/recv":
            pid = int(qs.get("id", [-1])[0])
            timeout = float(qs.get("timeout", ["5"])[0])
            with lock:
                if pid not in peers:
                    self._send_json({"error": "unknown peer"}, 404)
                    return
            msg = dequeue(pid, timeout)
            if msg is None:
                self.send_response(204)
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
            else:
                body = msg.encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Access-Control-Allow-Origin", "*")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
        else:
            self.send_error(404)

    def do_POST(self):
        parsed = urlparse(self.path)
        qs = parse_qs(parsed.query)

        if parsed.path == "/join":
            role = qs.get("role", [None])[0]
            pid = alloc_peer(role)
            if pid < 0:
                self._send_json({"error": "full"}, 409)
            else:
                mode = "host" if role == "host" else (
                    "viewer" if host_mode else "peer")
                print(f"[sig] {mode.capitalize()} {pid} joined "
                      f"({self.client_address[0]})")
                self._send_json({"id": pid})

        elif parsed.path == "/send":
            pid = int(qs.get("id", [-1])[0])
            length = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(length).decode() if length > 0 else ""

            if host_mode and pid == 0:
                # Host sending: route to specific viewer via &to=M
                to = int(qs.get("to", [-1])[0])
                if to < 0:
                    self._send_json({"error": "missing 'to' param"}, 400)
                    return
                try:
                    mtype = json.loads(body).get("type", "?")
                except (json.JSONDecodeError, AttributeError):
                    mtype = "?"
                print(f"[sig] Host -> Viewer {to}: {mtype}")
                enqueue(to, body)
                self._send_json({"ok": True})

            elif host_mode and pid > 0:
                # Viewer sending: route to host, inject "from" field
                try:
                    msg = json.loads(body)
                    mtype = msg.get("type", "?")
                    msg["from"] = pid
                    tagged = json.dumps(msg)
                except (json.JSONDecodeError, AttributeError):
                    mtype = "?"
                    tagged = body
                print(f"[sig] Viewer {pid} -> Host: {mtype}")
                enqueue(0, tagged)
                self._send_json({"ok": True})

            else:
                # Legacy 2-peer mode
                other = 1 - pid
                try:
                    mtype = json.loads(body).get("type", "?")
                except (json.JSONDecodeError, AttributeError):
                    mtype = "?"
                print(f"[sig] Peer {pid} -> Peer {other}: {mtype}")
                enqueue(other, body)
                self._send_json({"ok": True})

        elif parsed.path == "/leave":
            pid = int(qs.get("id", [-1])[0])
            remove_peer(pid)
            print(f"[sig] Peer {pid} left")
            self._send_json({"ok": True})

        else:
            self.send_error(404)


class ThreadedHTTPServer(HTTPServer):
    """Handle each request in a new thread (needed for long-poll /recv)."""
    def process_request(self, request, client_address):
        t = threading.Thread(target=self.process_request_thread,
                             args=(request, client_address))
        t.daemon = True
        t.start()

    def process_request_thread(self, request, client_address):
        try:
            self.finish_request(request, client_address)
        except Exception:
            self.handle_error(request, client_address)
        finally:
            self.shutdown_request(request)


def discovery_listener(http_port, discovery_port=19730):
    """UDP broadcast listener for ESP32 auto-discovery.

    Protocol (19 bytes fixed):
      Request:  "NANORTC_DISCOVER" (16B) + version(1B) + port(2B big-endian)
      Response: "NANORTC_FOUND\\0\\0\\0" (16B) + version(1B) + port(2B big-endian)
    """
    import socket as _socket
    sock = _socket.socket(_socket.AF_INET, _socket.SOCK_DGRAM)
    sock.setsockopt(_socket.SOL_SOCKET, _socket.SO_REUSEADDR, 1)
    try:
        sock.bind(("0.0.0.0", discovery_port))
    except OSError as e:
        print(f"[discovery] Cannot bind UDP port {discovery_port}: {e}")
        return
    print(f"[discovery] Listening on UDP port {discovery_port}")
    while True:
        try:
            data, addr = sock.recvfrom(64)
            if len(data) >= 19 and data[:16] == b"NANORTC_DISCOVER":
                resp = (b"NANORTC_FOUND\x00\x00\x00"
                        + bytes([1])
                        + http_port.to_bytes(2, "big"))
                sock.sendto(resp, addr)
                print(f"[discovery] Replied to {addr[0]}:{addr[1]}"
                      f" (HTTP port={http_port})")
        except Exception as e:
            print(f"[discovery] Error: {e}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="HTTP signaling relay")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--discovery-port", type=int, default=19730,
                        help="UDP port for ESP32 auto-discovery (0 to disable)")
    args = parser.parse_args()

    # Start UDP discovery listener in background
    if args.discovery_port > 0:
        dt = threading.Thread(target=discovery_listener,
                              args=(args.port, args.discovery_port),
                              daemon=True)
        dt.start()

    print(f"[sig] Signaling server on http://0.0.0.0:{args.port}")
    print(f"[sig]   Browser UI:  http://localhost:{args.port}/")
    print(f"[sig]   HTTP API:    POST /join, /send?id=N, /leave?id=N")
    print(f"[sig]                GET  /recv?id=N&timeout=5")
    print(f"[sig]   Host mode:   POST /join?role=host (enables multi-viewer)")

    server = ThreadedHTTPServer(("0.0.0.0", args.port), SigHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[sig] Shutting down")
        server.shutdown()
