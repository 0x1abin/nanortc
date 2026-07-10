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
LOG_DIR="$BUILD_DIR/fuzz-logs"

cd "$PROJECT_DIR"

# Detect clang with libFuzzer (prefer brew LLVM over AppleClang)
if [ -x /opt/homebrew/opt/llvm/bin/clang ]; then
    CLANG="/opt/homebrew/opt/llvm/bin/clang"
elif command -v clang >/dev/null 2>&1; then
    CLANG="$(command -v clang)"
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
# CMake cannot switch compilers in-place.  Drop only the generated fuzz build
# tree when a previous invocation used a different clang (for example,
# AppleClang before Homebrew LLVM was installed); corpora live outside it.
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
    CACHED_CLANG="$(sed -n 's/^CMAKE_C_COMPILER:[^=]*=//p' "$BUILD_DIR/CMakeCache.txt" | head -1)"
    if [ -n "$CACHED_CLANG" ] && [ "$CACHED_CLANG" != "$CLANG" ]; then
        echo "Compiler changed ($CACHED_CLANG -> $CLANG); refreshing $BUILD_DIR"
        cmake -E remove_directory "$BUILD_DIR"
    fi
fi
mkdir -p "$LOG_DIR"
if CC="$CLANG" cmake -B "$BUILD_DIR" \
    -DNANORTC_BUILD_FUZZ=ON \
    -DNANORTC_BUILD_TESTS=OFF \
    -DNANORTC_FEATURE_DATACHANNEL=ON \
    -DNANORTC_FEATURE_AUDIO=ON \
    -DNANORTC_FEATURE_VIDEO=ON \
    -DNANORTC_FEATURE_H265=ON \
    -DNANORTC_FEATURE_VIDEO_RATE_CONTROL=ON \
    -DNANORTC_FEATURE_VIDEO_REORDER=ON \
    -DNANORTC_FEATURE_VIDEO_NACK_RX=ON \
    -DNANORTC_FEATURE_VIDEO_FEC=ON \
    -DNANORTC_FEATURE_IPV6=ON \
    -DNANORTC_FEATURE_TURN=ON \
    -DNANORTC_CRYPTO="$CRYPTO" \
    -DCMAKE_C_COMPILER="$CLANG" \
    -DCMAKE_BUILD_TYPE=Debug \
    >"$LOG_DIR/configure.log" 2>&1; then
    tail -5 "$LOG_DIR/configure.log"
else
    cat "$LOG_DIR/configure.log"
    exit 1
fi

if cmake --build "$BUILD_DIR" -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)" \
    >"$LOG_DIR/build.log" 2>&1; then
    tail -5 "$LOG_DIR/build.log"
else
    cat "$LOG_DIR/build.log"
    exit 1
fi
echo ""

# Discover what CMake actually built so a newly-added fuzz_*.c harness is run
# automatically and feature-gated targets do not need duplicate shell lists.
mkdir -p "$CORPUS_DIR"
if [ "$TARGET" = "all" ]; then
    ALL_TARGETS="$(find "$BUILD_DIR/tests/fuzz" -maxdepth 1 -type f -perm -111 -name 'fuzz_*' \
        -exec basename {} \; | sort)"
else
    ALL_TARGETS="$TARGET"
fi

if [ -z "$ALL_TARGETS" ]; then
    echo "ERROR: no fuzz harnesses were built"
    exit 1
fi

FAIL=0
for t in $ALL_TARGETS; do
    BINARY="$BUILD_DIR/tests/fuzz/$t"
    if [ ! -x "$BINARY" ]; then
        echo "FAIL $t (not built or not executable — check the target name and feature flags)"
        FAIL=1
        continue
    fi

    # Corpus dir name is the target without the fuzz_ prefix
    CORPUS_NAME="${t#fuzz_}"
    TDIR="$CORPUS_DIR/$CORPUS_NAME"
    mkdir -p "$TDIR"
    LOG_FILE="$LOG_DIR/$t.log"

    echo "--- Running $t for ${DURATION}s ---"
    if "$BINARY" "$TDIR" \
        -max_total_time="$DURATION" \
        -max_len=4096 \
        -print_final_stats=1 \
        -artifact_prefix="$LOG_DIR/$t-" \
        >"$LOG_FILE" 2>&1; then
        tail -15 "$LOG_FILE"
        echo "PASS $t"
    else
        # Replay the complete failure log. The previous tail pipeline hid the
        # first sanitizer diagnostic and often omitted the reproducer path.
        cat "$LOG_FILE"
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
