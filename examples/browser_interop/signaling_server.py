#!/usr/bin/env python3
"""
nanortc browser_interop — HTTP signaling relay server

Two-peer message relay over pure HTTP. Zero external dependencies.
Supports browser (fetch), Linux C client, and ESP32 (esp_http_client).

API:
    POST /join          → {"id": N}           Register as a peer (max 2)
    POST /send?id=N     body=JSON             Relay message to other peer
    GET  /recv?id=N     → JSON or 204         Long-poll (query: timeout=5)
    POST /leave?id=N    → {"ok": true}        Unregister peer
    GET  /              → index.html           Browser UI

Message format (all transports):
    {"type":"offer",     "sdp":"v=0\\r\\n..."}
    {"type":"answer",    "sdp":"v=0\\r\\n..."}
    {"type":"candidate", "candidate":"candidate:..."}

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

# Two-peer message queues: peers[id] = {"msgs": [], "event": Event}
peers = {}
lock = threading.Lock()


def alloc_peer():
    with lock:
        for pid in (0, 1):
            if pid not in peers:
                peers[pid] = {"msgs": [], "event": threading.Event()}
                return pid
    return -1


def remove_peer(pid):
    with lock:
        peers.pop(pid, None)
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
            pid = alloc_peer()
            if pid < 0:
                self._send_json({"error": "full"}, 409)
            else:
                print(f"[sig] Peer {pid} joined ({self.client_address[0]})")
                self._send_json({"id": pid})

        elif parsed.path == "/send":
            pid = int(qs.get("id", [-1])[0])
            length = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(length).decode() if length > 0 else ""
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


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="HTTP signaling relay")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()

    print(f"[sig] Signaling server on http://0.0.0.0:{args.port}")
    print(f"[sig]   Browser UI:  http://localhost:{args.port}/")
    print(f"[sig]   HTTP API:    POST /join, /send?id=N, /leave?id=N")
    print(f"[sig]                GET  /recv?id=N&timeout=5")

    server = ThreadedHTTPServer(("0.0.0.0", args.port), SigHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[sig] Shutting down")
        server.shutdown()
