"""
aiortc-driven DataChannel interop tests.

Mirrors tests/interop/test_interop_dc.c one-to-one. Each test drives an
aiortc RTCPeerConnection (offerer) and a nanortc_peer_cli subprocess
(answerer) over localhost UDP, verifying DataChannel open + message
exchange in both directions.

The C file is the authoritative reference for scenarios; when porting
new cases, mirror them here by name.
"""

from __future__ import annotations

import asyncio
from contextlib import asynccontextmanager

import pytest
from aiortc import RTCPeerConnection, RTCSessionDescription

from harness import (
    NanortcPeer,
    wait_ice_gathering_complete,
)


# ---- Setup helper ------------------------------------------------------------


class _DcContext:
    """Holds the aiortc side of a connected pair plus the nanortc DC stream id."""

    def __init__(self, pc: RTCPeerConnection, channel, nanortc_peer: NanortcPeer, dc_id: int):
        self.pc = pc
        self.channel = channel
        self.peer = nanortc_peer
        self.dc_id = dc_id
        self.messages: asyncio.Queue = asyncio.Queue()

    async def recv(self, timeout: float = 5.0):
        return await asyncio.wait_for(self.messages.get(), timeout=timeout)


@asynccontextmanager
async def connected_pair(nanortc_peer: NanortcPeer, port: int, label: str = "test"):
    pc = RTCPeerConnection()
    channel = pc.createDataChannel(label)

    opened = asyncio.Event()
    msgs: asyncio.Queue = asyncio.Queue()

    @channel.on("open")
    def _on_open() -> None:  # noqa: F811
        opened.set()

    @channel.on("message")
    def _on_message(message) -> None:  # noqa: F811
        msgs.put_nowait(message)

    try:
        await nanortc_peer.init(port=port)

        offer = await pc.createOffer()
        await pc.setLocalDescription(offer)
        await wait_ice_gathering_complete(pc)

        offer_sdp = pc.localDescription.sdp
        answer_sdp = await nanortc_peer.set_offer(offer_sdp)
        await pc.setRemoteDescription(RTCSessionDescription(sdp=answer_sdp, type="answer"))

        # Wait for DC open on both sides
        if channel.readyState != "open":
            await asyncio.wait_for(opened.wait(), timeout=10.0)
        dc_id = await nanortc_peer.wait_dc_open(timeout=10.0)

        ctx = _DcContext(pc, channel, nanortc_peer, dc_id)
        ctx.messages = msgs
        yield ctx
    finally:
        try:
            await pc.close()
        except Exception:
            pass


# ---- Tests (mirrors test_interop_dc.c) ---------------------------------------


async def test_interop_handshake(nanortc_peer: NanortcPeer, alloc_port):
    """Full ICE + DTLS + SCTP handshake must complete."""
    port = alloc_port()
    async with connected_pair(nanortc_peer, port, "test") as ctx:
        # If we got here, aiortc's DC opened and nanortc reported DC_OPEN.
        # Reaching DC open means ICE, DTLS, and SCTP all completed successfully.
        assert ctx.channel.readyState == "open"
        assert ctx.dc_id >= 0


async def test_interop_dc_open(nanortc_peer: NanortcPeer, alloc_port):
    """DataChannel opens on both sides."""
    port = alloc_port()
    async with connected_pair(nanortc_peer, port, "interop-dc") as ctx:
        assert ctx.channel.readyState == "open"


async def test_interop_dc_string_aiortc_to_nanortc(nanortc_peer: NanortcPeer, alloc_port):
    """String message: aiortc → nanortc."""
    port = alloc_port()
    async with connected_pair(nanortc_peer, port, "echo") as ctx:
        ctx.channel.send("hello nanortc")
        is_string, payload = await nanortc_peer.wait_dc_message(timeout=5.0)
        assert is_string
        assert payload == b"hello nanortc"


async def test_interop_dc_string_nanortc_to_aiortc(nanortc_peer: NanortcPeer, alloc_port):
    """String message: nanortc → aiortc."""
    port = alloc_port()
    async with connected_pair(nanortc_peer, port, "echo") as ctx:
        await nanortc_peer.send_string("hello aiortc")
        msg = await ctx.recv(timeout=5.0)
        assert isinstance(msg, str)
        assert msg == "hello aiortc"


async def test_interop_dc_binary(nanortc_peer: NanortcPeer, alloc_port):
    """Binary message: aiortc → nanortc."""
    port = alloc_port()
    payload = bytes(range(256))
    async with connected_pair(nanortc_peer, port, "binary") as ctx:
        ctx.channel.send(payload)
        is_string, received = await nanortc_peer.wait_dc_message(timeout=5.0)
        assert not is_string
        assert received == payload


async def test_interop_dc_binary_nanortc_to_aiortc(nanortc_peer: NanortcPeer, alloc_port):
    """Binary message: nanortc → aiortc."""
    port = alloc_port()
    payload = bytes(range(256))
    async with connected_pair(nanortc_peer, port, "binary-rev") as ctx:
        await nanortc_peer.send_binary(payload)
        msg = await ctx.recv(timeout=5.0)
        assert isinstance(msg, (bytes, bytearray))
        assert bytes(msg) == payload


async def test_interop_dc_large_binary(nanortc_peer: NanortcPeer, alloc_port):
    """1000-byte binary exercises SCTP fragmentation path."""
    port = alloc_port()
    payload = bytes(i & 0xFF for i in range(1000))
    async with connected_pair(nanortc_peer, port, "large-bin") as ctx:
        ctx.channel.send(payload)
        is_string, received = await nanortc_peer.wait_dc_message(timeout=5.0)
        assert not is_string
        assert received == payload


async def test_interop_dc_single_byte(nanortc_peer: NanortcPeer, alloc_port):
    """Single-byte binary in both directions."""
    port = alloc_port()
    async with connected_pair(nanortc_peer, port, "single-byte") as ctx:
        # aiortc → nanortc: 0x42
        ctx.channel.send(bytes([0x42]))
        is_string, received = await nanortc_peer.wait_dc_message(timeout=5.0)
        assert not is_string
        assert received == bytes([0x42])

        # nanortc → aiortc: 0xAB
        await nanortc_peer.send_binary(bytes([0xAB]))
        msg = await ctx.recv(timeout=5.0)
        assert isinstance(msg, (bytes, bytearray))
        assert bytes(msg) == bytes([0xAB])


async def test_interop_dc_sequential_messages(nanortc_peer: NanortcPeer, alloc_port):
    """10 sequential string messages from aiortc, verified in order."""
    port = alloc_port()
    count = 10
    async with connected_pair(nanortc_peer, port, "sequential") as ctx:
        for i in range(count):
            ctx.channel.send(f"msg-{i}")
            is_string, payload = await nanortc_peer.wait_dc_message(timeout=5.0)
            assert is_string
            assert payload.decode("utf-8") == f"msg-{i}"


async def test_interop_dc_bidirectional(nanortc_peer: NanortcPeer, alloc_port):
    """Simultaneous sends from both sides."""
    port = alloc_port()
    async with connected_pair(nanortc_peer, port, "bidir") as ctx:
        ctx.channel.send("from-aiortc")
        await nanortc_peer.send_string("from-nano")

        is_string, nano_payload = await nanortc_peer.wait_dc_message(timeout=5.0)
        assert is_string
        assert nano_payload == b"from-aiortc"

        msg = await ctx.recv(timeout=5.0)
        assert isinstance(msg, str)
        assert msg == "from-nano"


async def test_interop_dc_echo_roundtrip(nanortc_peer: NanortcPeer, alloc_port):
    """aiortc sends a request; nanortc replies; aiortc receives the reply."""
    port = alloc_port()
    async with connected_pair(nanortc_peer, port, "echo-rt") as ctx:
        ctx.channel.send("echo-request")

        is_string, request = await nanortc_peer.wait_dc_message(timeout=5.0)
        assert is_string
        assert request == b"echo-request"

        await nanortc_peer.send_string("echo-reply")
        msg = await ctx.recv(timeout=5.0)
        assert isinstance(msg, str)
        assert msg == "echo-reply"
