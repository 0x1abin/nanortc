"""Signaling helpers for the aiortc ↔ nanortc_peer_cli handshake."""

from __future__ import annotations

import asyncio

from aiortc import RTCPeerConnection


async def wait_ice_gathering_complete(pc: RTCPeerConnection, timeout: float = 5.0) -> None:
    """Wait until the PC has finished ICE gathering.

    aiortc generally completes gathering synchronously inside setLocalDescription,
    but this helper defends against any future behavior change and is explicit about intent.
    """
    if pc.iceGatheringState == "complete":
        return
    done = asyncio.Event()

    @pc.on("icegatheringstatechange")
    def _on_state() -> None:
        if pc.iceGatheringState == "complete":
            done.set()

    await asyncio.wait_for(done.wait(), timeout=timeout)
