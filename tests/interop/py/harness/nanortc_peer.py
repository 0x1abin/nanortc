"""
Subprocess wrapper around nanortc_peer_cli.

Speaks the line-oriented wire protocol defined in ../protocol.md:
commands out on stdin, events in on stdout. Events arrive via a background
asyncio task and are queued for filtered consumption via wait_event().
"""

from __future__ import annotations

import asyncio
import base64
import sys
from dataclasses import dataclass, field
from typing import Optional


class NanortcPeerError(RuntimeError):
    """Raised when the CLI reports an error or times out."""


@dataclass
class _Event:
    verb: str
    args: list[str] = field(default_factory=list)


class NanortcPeer:
    """Async context manager wrapping the nanortc_peer_cli subprocess."""

    def __init__(self, cli_path: str) -> None:
        self._cli_path = cli_path
        self._proc: Optional[asyncio.subprocess.Process] = None
        self._events: asyncio.Queue[_Event] = asyncio.Queue()
        self._reader_task: Optional[asyncio.Task] = None
        self._stderr_task: Optional[asyncio.Task] = None
        self._dc_id: Optional[int] = None
        self._stopped = False

    # ---- Lifecycle ----

    async def __aenter__(self) -> "NanortcPeer":
        self._proc = await asyncio.create_subprocess_exec(
            self._cli_path,
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        self._reader_task = asyncio.create_task(self._read_events())
        self._stderr_task = asyncio.create_task(self._drain_stderr())
        return self

    async def __aexit__(self, exc_type, exc, tb) -> None:
        await self.shutdown()

    async def shutdown(self) -> None:
        if self._stopped or self._proc is None:
            return
        self._stopped = True
        try:
            if self._proc.returncode is None:
                try:
                    self._proc.stdin.write(b"SHUTDOWN\n")  # type: ignore[union-attr]
                    await self._proc.stdin.drain()  # type: ignore[union-attr]
                except (BrokenPipeError, ConnectionResetError):
                    pass
                try:
                    await asyncio.wait_for(self._proc.wait(), timeout=2.0)
                except asyncio.TimeoutError:
                    self._proc.kill()
                    await self._proc.wait()
        finally:
            if self._reader_task is not None:
                self._reader_task.cancel()
                try:
                    await self._reader_task
                except (asyncio.CancelledError, Exception):
                    pass
            if self._stderr_task is not None:
                self._stderr_task.cancel()
                try:
                    await self._stderr_task
                except (asyncio.CancelledError, Exception):
                    pass

    # ---- Commands ----

    async def init(self, port: int) -> None:
        await self._send(f"INIT {port}")
        ev = await self.wait_event("READY", timeout=5.0)
        assert ev.verb == "READY"

    async def set_offer(self, sdp: str) -> str:
        encoded = base64.b64encode(sdp.encode("utf-8")).decode("ascii")
        await self._send(f"SET_OFFER {encoded}")
        ev = await self.wait_event("LOCAL_ANSWER", timeout=10.0)
        if not ev.args:
            raise NanortcPeerError("LOCAL_ANSWER missing base64 payload")
        return base64.b64decode(ev.args[0]).decode("utf-8")

    async def add_candidate(self, candidate_line: str) -> None:
        encoded = base64.b64encode(candidate_line.encode("utf-8")).decode("ascii")
        await self._send(f"ADD_CANDIDATE {encoded}")

    async def send_string(self, data: str, dc_id: Optional[int] = None) -> None:
        target = dc_id if dc_id is not None else self._require_dc_id()
        encoded = base64.b64encode(data.encode("utf-8")).decode("ascii")
        await self._send(f"DC_SEND_STRING {target} {encoded}")

    async def send_binary(self, data: bytes, dc_id: Optional[int] = None) -> None:
        target = dc_id if dc_id is not None else self._require_dc_id()
        encoded = base64.b64encode(data).decode("ascii")
        await self._send(f"DC_SEND_BINARY {target} {encoded}")

    # ---- Event waiting ----

    async def wait_event(self, verb: str, timeout: float = 10.0) -> _Event:
        """Block until an event of the given verb arrives, discarding earlier unrelated events."""
        deadline = asyncio.get_event_loop().time() + timeout
        while True:
            remaining = deadline - asyncio.get_event_loop().time()
            if remaining <= 0:
                raise NanortcPeerError(f"timeout waiting for {verb}")
            try:
                ev = await asyncio.wait_for(self._events.get(), timeout=remaining)
            except asyncio.TimeoutError:
                raise NanortcPeerError(f"timeout waiting for {verb}")
            if ev.verb == "ERROR":
                msg = base64.b64decode(ev.args[0]).decode("utf-8", errors="replace") if ev.args else "<no-msg>"
                raise NanortcPeerError(f"CLI error: {msg}")
            if ev.verb == verb:
                return ev
            # Side-channel: remember first DC open so send_* can target it without a caller-supplied id
            if ev.verb == "DC_OPEN" and self._dc_id is None and ev.args:
                try:
                    self._dc_id = int(ev.args[0])
                except ValueError:
                    pass

    async def wait_dc_open(self, timeout: float = 10.0) -> int:
        ev = await self.wait_event("DC_OPEN", timeout=timeout)
        if not ev.args:
            raise NanortcPeerError("DC_OPEN missing id")
        dc_id = int(ev.args[0])
        self._dc_id = dc_id
        return dc_id

    async def wait_ice_connected(self, timeout: float = 10.0) -> None:
        deadline = asyncio.get_event_loop().time() + timeout
        while True:
            remaining = deadline - asyncio.get_event_loop().time()
            if remaining <= 0:
                raise NanortcPeerError("timeout waiting for ICE connected")
            ev = await asyncio.wait_for(self._events.get(), timeout=remaining)
            if ev.verb == "ICE_STATE" and ev.args and ev.args[0] == "connected":
                return
            if ev.verb == "ERROR":
                msg = base64.b64decode(ev.args[0]).decode("utf-8", errors="replace") if ev.args else "<no-msg>"
                raise NanortcPeerError(f"CLI error: {msg}")

    async def wait_connected(self, timeout: float = 10.0) -> None:
        await self.wait_event("CONNECTED", timeout=timeout)

    async def wait_dc_message(self, timeout: float = 10.0) -> tuple[bool, bytes]:
        """Returns (is_string, payload_bytes)."""
        ev = await self.wait_event("DC_MESSAGE", timeout=timeout)
        if len(ev.args) < 3:
            raise NanortcPeerError(f"DC_MESSAGE missing fields: {ev.args}")
        kind = ev.args[1]
        payload = base64.b64decode(ev.args[2])
        return (kind == "STRING"), payload

    # ---- Internals ----

    def _require_dc_id(self) -> int:
        if self._dc_id is None:
            raise NanortcPeerError("no DataChannel id known (wait_dc_open first)")
        return self._dc_id

    async def _send(self, line: str) -> None:
        if self._proc is None or self._proc.stdin is None:
            raise NanortcPeerError("peer not started")
        self._proc.stdin.write(line.encode("ascii") + b"\n")
        await self._proc.stdin.drain()

    async def _read_events(self) -> None:
        assert self._proc is not None and self._proc.stdout is not None
        while True:
            line = await self._proc.stdout.readline()
            if not line:
                return
            text = line.decode("ascii", errors="replace").rstrip("\r\n")
            if not text:
                continue
            parts = text.split(" ")
            await self._events.put(_Event(verb=parts[0], args=parts[1:]))

    async def _drain_stderr(self) -> None:
        """Relay CLI stderr into pytest stderr for failure triage."""
        assert self._proc is not None and self._proc.stderr is not None
        while True:
            line = await self._proc.stderr.readline()
            if not line:
                return
            sys.stderr.write(f"[peer_cli] {line.decode('utf-8', errors='replace')}")
