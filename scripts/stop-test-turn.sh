#!/usr/bin/env bash
# Stop the local coturn started by start-test-turn.sh.
#
# Usage: ./scripts/stop-test-turn.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMPOSE_DIR="${SCRIPT_DIR}/../tests/interop/turn-server"

if docker compose version >/dev/null 2>&1; then
    COMPOSE=(docker compose)
elif command -v docker-compose >/dev/null 2>&1; then
    COMPOSE=(docker-compose)
else
    echo "error: neither 'docker compose' nor 'docker-compose' is available" >&2
    exit 1
fi

cd "${COMPOSE_DIR}"
"${COMPOSE[@]}" down
echo "[stop-test-turn] coturn stopped"
