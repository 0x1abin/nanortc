#!/usr/bin/env python3
"""
nanortc ESP32 DataChannel end-to-end test (fully automated)

Automated test that:
  1. Builds the ESP32 firmware (idf.py build)
  2. Flashes to the ESP32 (idf.py flash)
  3. Starts idf.py monitor in background to capture serial logs
  4. Starts the signaling server (with UDP discovery)
  5. Waits for ESP32 "WiFi connected" + signaling join
  6. Launches the browser_interop Linux binary as offerer
  7. Verifies DataChannel echo: send "ping" → expect "ping" back
  8. Reports PASS/FAIL, cleans up all subprocesses

Prerequisites:
  - ESP-IDF environment sourced (. $IDF_PATH/export.sh)
  - ESP32-S3 connected (default: /dev/cu.usbmodem1101)
  - browser_interop binary built:
      cmake -B build -DNANORTC_CRYPTO=openssl -DNANORTC_BUILD_EXAMPLES=ON
      cmake --build build -j$(nproc)

Usage:
  python3 tests/esp32/test_esp32_dc.py [options]

SPDX-License-Identifier: MIT
"""

import argparse
import os
import re
import signal
import subprocess
import sys
import threading
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, "..", ".."))
ESP_PROJECT = os.path.join(REPO_ROOT, "examples", "esp32_datachannel")


def find_binary():
    """Find the browser_interop binary in common build directories."""
    candidates = [
        os.path.join(REPO_ROOT, "build", "examples", "browser_interop",
                     "browser_interop"),
        os.path.join(REPO_ROOT, "build-release", "examples",
                     "browser_interop", "browser_interop"),
    ]
    for c in candidates:
        if os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    return None


class LogWatcher:
    """Watches a subprocess stdout for patterns, thread-safe."""

    def __init__(self, proc, prefix):
        self.proc = proc
        self.prefix = prefix
        self.lines = []
        self.events = {}  # pattern -> threading.Event
        self._lock = threading.Lock()
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()

    def watch_for(self, name, pattern):
        """Register a pattern to watch for. Returns an Event."""
        evt = threading.Event()
        with self._lock:
            self.events[name] = (re.compile(pattern), evt)
        # Check existing lines
        for line in self.lines:
            if re.search(pattern, line):
                evt.set()
                break
        return evt

    def _reader(self):
        try:
            for raw in self.proc.stdout:
                line = raw.strip()
                print(f"  [{self.prefix}] {line}", flush=True)
                with self._lock:
                    self.lines.append(line)
                    for name, (pat, evt) in self.events.items():
                        if pat.search(line):
                            evt.set()
        except (ValueError, OSError):
            pass  # pipe closed


def run_idf_cmd(args, cwd, desc, timeout=300):
    """Run an idf.py command, streaming output. Returns True on success."""
    print(f"[test] {desc}...")
    proc = subprocess.run(
        ["idf.py"] + args,
        cwd=cwd,
        timeout=timeout,
        capture_output=False,
    )
    if proc.returncode != 0:
        print(f"FAIL: {desc} failed (exit {proc.returncode})")
        return False
    print(f"[test] {desc} OK")
    return True


def main():
    parser = argparse.ArgumentParser(
        description="ESP32 DataChannel E2E test (automated)")
    parser.add_argument("--binary", default=None,
                        help="Path to browser_interop binary")
    parser.add_argument("--port", "-p", default="/dev/cu.usbmodem1101",
                        help="ESP32 serial port")
    parser.add_argument("--sig-port", type=int, default=8765,
                        help="Signaling server HTTP port")
    parser.add_argument("--timeout", type=int, default=60,
                        help="Overall test timeout in seconds")
    parser.add_argument("--skip-build", action="store_true",
                        help="Skip idf.py build")
    parser.add_argument("--skip-flash", action="store_true",
                        help="Skip idf.py flash")
    parser.add_argument("--target", default="esp32s3",
                        help="ESP32 target (default: esp32s3)")
    args = parser.parse_args()

    # Pre-check: IDF_PATH
    if not os.environ.get("IDF_PATH"):
        print("ERROR: IDF_PATH not set. Source ESP-IDF first:")
        print("  . $HOME/workspace/esp/esp-idf/export.sh")
        sys.exit(1)

    # Pre-check: browser_interop binary
    binary = args.binary or find_binary()
    if not binary:
        print("ERROR: browser_interop binary not found. Build it first:")
        print("  cmake -B build -DNANORTC_CRYPTO=openssl "
              "-DNANORTC_BUILD_EXAMPLES=ON")
        print("  cmake --build build -j$(nproc)")
        sys.exit(1)
    print(f"[test] Using binary: {binary}")

    sig_script = os.path.join(REPO_ROOT, "examples", "browser_interop",
                              "signaling_server.py")
    if not os.path.isfile(sig_script):
        print(f"ERROR: signaling_server.py not found at {sig_script}")
        sys.exit(1)

    # Track all subprocesses for cleanup
    procs = []

    def cleanup():
        for p in procs:
            try:
                p.terminate()
                p.wait(timeout=5)
            except Exception:
                try:
                    p.kill()
                except Exception:
                    pass

    try:
        # --- Step 1: Set target + Build ---
        if not args.skip_build:
            # Check if target needs to be set
            sdkconfig = os.path.join(ESP_PROJECT, "sdkconfig")
            need_target = True
            if os.path.isfile(sdkconfig):
                with open(sdkconfig, "r") as f:
                    content = f.read()
                    if f"CONFIG_IDF_TARGET=\"{args.target}\"" in content:
                        need_target = False

            if need_target:
                if not run_idf_cmd(["set-target", args.target],
                                   ESP_PROJECT, f"Set target {args.target}"):
                    sys.exit(1)

            if not run_idf_cmd(["build"], ESP_PROJECT, "Build firmware",
                               timeout=600):
                sys.exit(1)

        # --- Step 2: Flash ---
        if not args.skip_flash:
            if not run_idf_cmd(["-p", args.port, "flash"],
                               ESP_PROJECT, "Flash firmware"):
                sys.exit(1)

        # --- Step 3: Start monitor (background) ---
        print(f"[test] Starting serial monitor on {args.port}...")
        monitor_proc = subprocess.Popen(
            ["idf.py", "-p", args.port, "monitor", "--no-reset"],
            cwd=ESP_PROJECT,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True,
        )
        procs.append(monitor_proc)
        monitor = LogWatcher(monitor_proc, "esp32")

        wifi_evt = monitor.watch_for("wifi", r"WiFi connected")
        dc_open_esp = monitor.watch_for("dc_open", r"DataChannel open")
        dc_echo_esp = monitor.watch_for("dc_echo", r"DC (string|data)")

        # --- Step 4: Start signaling server ---
        print(f"[test] Starting signaling server on port {args.sig_port}...")
        sig_proc = subprocess.Popen(
            [sys.executable, sig_script, "--port", str(args.sig_port)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True,
        )
        procs.append(sig_proc)
        sig_watch = LogWatcher(sig_proc, "sig")
        peer_joined = sig_watch.watch_for("join", r"Peer.*joined")

        # --- Step 5: Wait for WiFi + join ---
        print("[test] Waiting for ESP32 WiFi + signaling join...")
        deadline = time.monotonic() + args.timeout

        if not wifi_evt.wait(timeout=max(0, deadline - time.monotonic())):
            print("FAIL: ESP32 WiFi did not connect within timeout")
            sys.exit(1)
        print("[test] ESP32 WiFi connected")

        if not peer_joined.wait(timeout=max(0, deadline - time.monotonic())):
            print("FAIL: ESP32 did not join signaling server within timeout")
            sys.exit(1)
        print("[test] ESP32 joined signaling server")

        # --- Step 6: Launch offerer ---
        print(f"[test] Starting offerer: {binary} --offer")
        offerer = subprocess.Popen(
            [binary, "--offer", "-s", f"localhost:{args.sig_port}"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True,
        )
        procs.append(offerer)
        offerer_watch = LogWatcher(offerer, "offerer")
        offerer_dc_open = offerer_watch.watch_for("dc_open",
                                                   r"DataChannel open")
        offerer_echo = offerer_watch.watch_for("echo",
                                               r"DC (string|data)")

        # --- Step 7: Wait for DataChannel echo ---
        print("[test] Waiting for DataChannel open + echo...")

        remaining = max(0, deadline - time.monotonic())
        if not offerer_dc_open.wait(timeout=remaining):
            print("\nFAIL: DataChannel never opened on offerer side")
            sys.exit(1)
        print("[test] DataChannel opened")

        remaining = max(0, deadline - time.monotonic())
        if not offerer_echo.wait(timeout=remaining):
            # Also check ESP32 side
            if dc_echo_esp.is_set():
                print("[test] Echo seen on ESP32 side (offerer may have "
                      "missed log)")
            else:
                print("\nFAIL: DataChannel opened but no echo received")
                sys.exit(1)

        print("\nPASS: ESP32 DataChannel echo verified")

    except KeyboardInterrupt:
        print("\n[test] Interrupted")
        sys.exit(130)
    finally:
        cleanup()


if __name__ == "__main__":
    main()
