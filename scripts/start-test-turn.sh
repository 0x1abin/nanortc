#!/usr/bin/env bash
# Start a local coturn for nanortc relay-only interop tests.
#
# Usage: ./scripts/start-test-turn.sh
#
# Endpoint: turn:127.0.0.1:3478
# Credentials: testuser / testpass (long-term, realm=nanortc-test)
#
# Stop with ./scripts/stop-test-turn.sh

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

# Wait for coturn to actually accept UDP on 3478. We probe with a single
# zero-byte send; coturn drops it but the kernel returns success once the
# container is listening.
echo -n "[start-test-turn] waiting for coturn..."
PROBE_PY=$(cat <<'PY'
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(0.5)
try:
    s.sendto(b"\x00", ("127.0.0.1", 3478))
    sys.exit(0)
except Exception:
    sys.exit(1)
PY
)
for _ in $(seq 1 50); do
    if python3 -c "${PROBE_PY}" 2>/dev/null; then
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
