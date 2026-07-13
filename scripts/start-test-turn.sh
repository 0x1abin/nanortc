#!/usr/bin/env bash
# Start a local coturn for nanortc relay-only interop tests.
#
# Usage: ./scripts/start-test-turn.sh
#
# Endpoint: turn:127.0.0.1:3478 (loopback; coturn runs in network_mode=host)
# Credentials: testuser / testpass (long-term, realm=nanortc-test)
#
# Stop with ./scripts/stop-test-turn.sh
#
# Note: the libdc-relay-only direction (test_interop_turn_relay) works fully
# over loopback. The nanortc-as-TURN-client direction
# (test_interop_turn_relay_nanortc) auto-skips its strict assertions when
# the configured TURN URL resolves to a loopback address — libjuice/libdc
# filter loopback host candidates per RFC 8838, so coturn cannot observe a
# permission-matching peer source for libdc's actual traffic. To exercise
# the strict relay path locally, point NANORTC_TURN_URL at a non-loopback
# IP that this host listens on (e.g. its LAN address). CI does this
# automatically via the workflow's host-IP detection step.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMPOSE_DIR="${SCRIPT_DIR}/../tests/interop/turn-server"

if ! command -v docker >/dev/null 2>&1; then
    echo "error: docker is not installed or not on PATH" >&2
    exit 1
fi

# Pick whichever compose interface is available.
if docker compose version >/dev/null 2>&1; then
    COMPOSE=(docker compose)
elif command -v docker-compose >/dev/null 2>&1; then
    COMPOSE=(docker-compose)
else
    echo "error: neither 'docker compose' nor 'docker-compose' is available" >&2
    exit 1
fi

cd "${COMPOSE_DIR}"
"${COMPOSE[@]}" up -d

# Wait for coturn to actually accept TCP connections on 3478. coturn runs in
# network_mode=host (see docker-compose.yml), so it listens on every host
# interface including loopback. A successful /dev/tcp open confirms the coturn
# process is listening instead of merely confirming that a UDP send was
# accepted; the relay tests themselves still exercise the UDP listener.
echo -n "[start-test-turn] waiting for coturn..."
for _ in $(seq 1 50); do
    if (exec 3<>/dev/tcp/127.0.0.1/3478) 2>/dev/null; then
        echo " ready."
        echo "[start-test-turn] coturn is up at 127.0.0.1:3478"
        echo "[start-test-turn] credentials: testuser / testpass (realm=nanortc-test)"
        echo "[start-test-turn] run: ctest --test-dir build-interop -L turn-relay --output-on-failure"
        exit 0
    fi
    sleep 0.2
done

echo
echo "[start-test-turn] coturn did not become ready within 10 s" >&2
"${COMPOSE[@]}" logs --tail=30 coturn >&2 || true
exit 1
