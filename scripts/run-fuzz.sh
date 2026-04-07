#!/bin/bash
# run-fuzz.sh — Build and run nanortc fuzz harnesses
#
# Usage:
#   ./scripts/run-fuzz.sh                    # Run all harnesses for 30s each
#   ./scripts/run-fuzz.sh 300                # Run all harnesses for 5min each
#   ./scripts/run-fuzz.sh 30 fuzz_stun       # Run one harness for 30s
#
# Requires: clang with libFuzzer support, mbedtls or openssl dev headers
#
# SPDX-License-Identifier: MIT

set -euo pipefail

DURATION="${1:-30}"
TARGET="${2:-all}"
BUILD_DIR="build-fuzz"
CORPUS_DIR="tests/fuzz/corpus"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

cd "$PROJECT_DIR"

# Detect clang with libFuzzer (prefer brew LLVM over AppleClang)
if [ -x /opt/homebrew/opt/llvm/bin/clang ]; then
    CLANG="/opt/homebrew/opt/llvm/bin/clang"
elif command -v clang >/dev/null 2>&1; then
    CLANG="clang"
else
    echo "ERROR: clang not found"
    exit 1
fi
echo "Using: $CLANG"

# Detect crypto backend
if pkg-config --exists openssl 2>/dev/null; then
    CRYPTO="openssl"
elif pkg-config --exists mbedtls 2>/dev/null; then
    CRYPTO="mbedtls"
else
    CRYPTO="mbedtls"
fi

echo "=== nanortc fuzz testing ==="
echo "Duration per harness: ${DURATION}s"
echo "Crypto backend: ${CRYPTO}"
echo ""

# Build fuzz targets
echo "--- Building fuzz targets ---"
CC="$CLANG" cmake -B "$BUILD_DIR" \
    -DNANORTC_BUILD_FUZZ=ON \
    -DNANORTC_BUILD_TESTS=OFF \
    -DNANORTC_FEATURE_DATACHANNEL=ON \
    -DNANORTC_FEATURE_AUDIO=ON \
    -DNANORTC_FEATURE_VIDEO=ON \
    -DNANORTC_FEATURE_IPV6=ON \
    -DNANORTC_CRYPTO="$CRYPTO" \
    -DCMAKE_C_COMPILER="$CLANG" \
    -DCMAKE_BUILD_TYPE=Debug \
    2>&1 | tail -5

cmake --build "$BUILD_DIR" -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)" 2>&1 | tail -5
echo ""

# Create corpus directories
mkdir -p "$CORPUS_DIR"/{stun,sctp,sdp,rtp,h264,addr,bwe}

# All fuzz targets
ALL_TARGETS="fuzz_stun fuzz_sctp fuzz_sdp fuzz_rtp fuzz_h264 fuzz_addr fuzz_bwe"

if [ "$TARGET" != "all" ]; then
    ALL_TARGETS="$TARGET"
fi

FAIL=0
for t in $ALL_TARGETS; do
    BINARY="$BUILD_DIR/tests/fuzz/$t"
    if [ ! -f "$BINARY" ]; then
        echo "SKIP $t (not built — check feature flags)"
        continue
    fi

    # Corpus dir name is the target without the fuzz_ prefix
    CORPUS_NAME="${t#fuzz_}"
    TDIR="$CORPUS_DIR/$CORPUS_NAME"
    mkdir -p "$TDIR"

    echo "--- Running $t for ${DURATION}s ---"
    if "$BINARY" "$TDIR" \
        -max_total_time="$DURATION" \
        -max_len=4096 \
        -print_final_stats=1 \
        2>&1 | tail -15; then
        echo "PASS $t"
    else
        echo "FAIL $t"
        FAIL=1
    fi
    echo ""
done

if [ "$FAIL" -eq 0 ]; then
    echo "=== All fuzz harnesses passed ==="
else
    echo "=== Some fuzz harnesses FAILED ==="
    exit 1
fi
