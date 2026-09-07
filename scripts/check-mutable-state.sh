#!/usr/bin/env bash
# Inspect both file-local and exported mutable symbols in the Sans I/O core.
# Crypto providers live outside src/ and own platform-specific state.
set -euo pipefail
nm -A "$1" | awk '
$1 ~ /:nano_[^:]*[.]c[.]o:/ && $2 ~ /^[bBdD]$/ { print; bad = 1 }
END { exit bad }'
