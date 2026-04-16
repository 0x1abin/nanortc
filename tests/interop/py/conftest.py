"""Pytest fixtures shared across the aiortc-based interop suite."""

from __future__ import annotations

import os

import pytest
import pytest_asyncio

# --- Force aiortc to use loopback only ---------------------------------------
#
# aioice's get_host_addresses() excludes 127.0.0.1 and ::1 from host candidates
# (aioice/ice.py:81). For our localhost-only interop tests we need the opposite:
# only loopback, no real-network interfaces. Without this patch, aiortc would
# gather real-interface candidates whose STUN responses arrive at nanortc's
# wildcard socket with the real IP as source, and nanortc's replies would then
# use the same real IP as source — causing aioice's "source address mismatch"
# ICE check to fail every pair.
import aioice.ice  # noqa: E402


def _loopback_only_host_addresses(use_ipv4: bool, use_ipv6: bool) -> list[str]:
    addrs: list[str] = []
    if use_ipv4:
        addrs.append("127.0.0.1")
    if use_ipv6:
        addrs.append("::1")
    return addrs


aioice.ice.get_host_addresses = _loopback_only_host_addresses

from harness import NanortcPeer  # noqa: E402


INTEROP_PY_PORT_BASE = 19500
_next_port = INTEROP_PY_PORT_BASE


def _alloc_port() -> int:
    global _next_port
    port = _next_port
    _next_port += 1
    return port


@pytest.fixture(scope="session")
def cli_path() -> str:
    path = os.environ.get("NANORTC_PEER_CLI")
    if not path:
        pytest.fail(
            "NANORTC_PEER_CLI env var not set; ctest wires this to "
            "$<TARGET_FILE:nanortc_peer_cli>"
        )
    if not os.access(path, os.X_OK):
        pytest.fail(f"NANORTC_PEER_CLI={path} is not executable")
    return path


@pytest.fixture
def alloc_port():
    return _alloc_port


@pytest_asyncio.fixture
async def nanortc_peer(cli_path: str):
    peer = NanortcPeer(cli_path)
    async with peer:
        yield peer
