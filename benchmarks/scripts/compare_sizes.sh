#!/usr/bin/env bash
#
# nanortc — Size comparison: nanortc vs competitors
#
# Usage: ./benchmarks/scripts/compare_sizes.sh              # nanortc only + per-module
#        ./benchmarks/scripts/compare_sizes.sh --ldc         # Also build libdatachannel
#
# Output: Markdown tables (JSON on stdout, human-readable on stderr)
#
# Measures:
#   1. nanortc per-feature-combo .text + sizeof (extends measure-sizes.sh)
#   2. nanortc per-module .text breakdown (nm --size-sort on each .o)
#   3. Source line counts (cloc/tokei or wc -l fallback)
#   4. Optionally: libdatachannel .text + dependency total

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

BUILD_LDC=false
if [[ "${1:-}" == "--ldc" ]]; then
    BUILD_LDC=true
fi

NCPU=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

IS_MACOS=false
if [[ "$(uname)" == "Darwin" ]]; then
    IS_MACOS=true
fi

format_kb() {
    local bytes="$1"
    if [ "$bytes" = "?" ] || [ -z "$bytes" ]; then echo "?"; return; fi
    echo "$bytes" | awk '{printf "%.1f KB", $1/1024}'
}

# Get .text size from a .o file
get_text_size_o() {
    local obj="$1"
    if $IS_MACOS; then
        size -m "$obj" 2>/dev/null | grep '__TEXT, __text' | awk -F: '{s+=$2}END{print s+0}'
    else
        size "$obj" 2>/dev/null | awk 'NR>1{s+=$1}END{print s+0}'
    fi
}

# Get .text from static library
get_text_size_lib() {
    local lib="$1"
    if $IS_MACOS; then
        size -m "$lib" 2>/dev/null | grep '__TEXT, __text' | awk -F: '{s+=$2}END{print s+0}'
    else
        size "$lib" 2>/dev/null | awk 'NR>1{s+=$1}END{print s+0}'
    fi
}

# --- Auto-detect crypto ---
if pkg-config --exists openssl 2>/dev/null || [ -f /usr/include/openssl/ssl.h ]; then
    CRYPTO_FLAG="-DNANORTC_CRYPTO=openssl"
    CRYPTO_NAME="OpenSSL"
elif pkg-config --exists mbedtls 2>/dev/null || [ -f /usr/include/mbedtls/ssl.h ]; then
    CRYPTO_FLAG="-DNANORTC_CRYPTO=mbedtls"
    CRYPTO_NAME="mbedtls"
else
    echo "ERROR: neither openssl nor mbedtls found" >&2
    exit 1
fi

# =====================================================================
# Section 1: nanortc per-feature-combo sizes
# =====================================================================
echo "=== nanortc feature combos ===" >&2

COMBO_NAMES=(  CORE_ONLY    DATA    AUDIO_ONLY    AUDIO    MEDIA_ONLY    MEDIA)
COMBO_LABELS=( "Core only"  "DataChannel"  "Audio only"  "DC + Audio"  "Media only"  "Full media")
COMBO_SHORT=(
    "DC=OFF AUDIO=OFF VIDEO=OFF"
    "DC=ON"
    "DC=OFF AUDIO=ON"
    "DC=ON AUDIO=ON"
    "DC=OFF AUDIO=ON VIDEO=ON"
    "DC=ON AUDIO=ON VIDEO=ON"
)
COMBO_CMAKE=(
    "-DNANORTC_FEATURE_DATACHANNEL=OFF -DNANORTC_FEATURE_AUDIO=OFF -DNANORTC_FEATURE_VIDEO=OFF"
    "-DNANORTC_FEATURE_DATACHANNEL=ON  -DNANORTC_FEATURE_AUDIO=OFF -DNANORTC_FEATURE_VIDEO=OFF"
    "-DNANORTC_FEATURE_DATACHANNEL=OFF -DNANORTC_FEATURE_AUDIO=ON  -DNANORTC_FEATURE_VIDEO=OFF"
    "-DNANORTC_FEATURE_DATACHANNEL=ON  -DNANORTC_FEATURE_AUDIO=ON  -DNANORTC_FEATURE_VIDEO=OFF"
    "-DNANORTC_FEATURE_DATACHANNEL=OFF -DNANORTC_FEATURE_AUDIO=ON  -DNANORTC_FEATURE_VIDEO=ON"
    "-DNANORTC_FEATURE_DATACHANNEL=ON  -DNANORTC_FEATURE_AUDIO=ON  -DNANORTC_FEATURE_VIDEO=ON"
)

TEXT_SIZES=()
RAM_SIZES=()

for i in "${!COMBO_NAMES[@]}"; do
    combo="${COMBO_NAMES[$i]}"
    cmake_flags="${COMBO_CMAKE[$i]}"
    build_dir="$ROOT/.cache/bench-size-${combo}"
    rm -rf "$build_dir"

    echo "  Building $combo ..." >&2
    eval cmake -B "$build_dir" $cmake_flags $CRYPTO_FLAG \
          -DCMAKE_BUILD_TYPE=Release -DNANORTC_BUILD_TESTS=OFF > /dev/null 2>&1
    cmake --build "$build_dir" -j"$NCPU" > /dev/null 2>&1

    lib="$build_dir/libnanortc.a"
    text_bytes=$(get_text_size_lib "$lib" 2>/dev/null || echo "?")
    TEXT_SIZES+=("${text_bytes}")

    # sizeof probe
    sizeof_prog="$build_dir/_sizeof.c"
    cat > "$sizeof_prog" << 'EOF'
#include <stdio.h>
#include "nanortc.h"
int main(void) { printf("%zu\n", sizeof(nanortc_t)); return 0; }
EOF

    defines=""
    for flag in $cmake_flags; do
        defines="$defines $(echo "$flag" | sed 's/=ON$/=1/' | sed 's/=OFF$/=0/')"
    done

    sizeof_bin="$build_dir/_sizeof"
    cc -I"$ROOT/include" -I"$ROOT/src" -I"$ROOT/crypto" $defines \
       "$sizeof_prog" -o "$sizeof_bin" 2>/dev/null || true

    if [ -x "$sizeof_bin" ]; then
        RAM_SIZES+=("$("$sizeof_bin")")
    else
        RAM_SIZES+=("?")
    fi
done

echo "" >&2
echo "## nanortc Feature Combo Sizes"
echo ""
echo "| Configuration | Flash (.text) | RAM (sizeof) | Flags |"
echo "|---|---|---|---|"
for i in "${!COMBO_NAMES[@]}"; do
    echo "| ${COMBO_LABELS[$i]} | $(format_kb "${TEXT_SIZES[$i]}") | $(format_kb "${RAM_SIZES[$i]}") | ${COMBO_SHORT[$i]} |"
done
echo ""

# =====================================================================
# Section 2: Per-module .text breakdown (full media build)
# =====================================================================
echo "=== per-module breakdown ===" >&2

full_build="$ROOT/.cache/bench-size-MEDIA"
echo ""
echo "## Per-Module .text Breakdown (Full Media)"
echo ""
echo "| Module | .text (bytes) | .text (KB) |"
echo "|---|---|---|"

for obj in "$full_build"/CMakeFiles/nanortc.dir/src/*.c.o; do
    if [ -f "$obj" ]; then
        name=$(basename "$obj" .c.o)
        bytes=$(get_text_size_o "$obj")
        kb=$(echo "$bytes" | awk '{printf "%.1f", $1/1024}')
        echo "| $name | $bytes | $kb KB |"
        printf '{"module":"%s","text_bytes":%s}\n' "$name" "$bytes"
    fi
done

# Also measure crypto backend
for obj in "$full_build"/CMakeFiles/nanortc.dir/crypto/*.c.o; do
    if [ -f "$obj" ]; then
        name=$(basename "$obj" .c.o)
        bytes=$(get_text_size_o "$obj")
        kb=$(echo "$bytes" | awk '{printf "%.1f", $1/1024}')
        echo "| $name | $bytes | $kb KB |"
    fi
done

echo ""

# =====================================================================
# Section 3: Source line counts
# =====================================================================
echo "=== source metrics ===" >&2

sloc_core=$(cat src/*.c src/*.h | wc -l | tr -d ' ')
sloc_include=$(cat include/*.h | wc -l | tr -d ' ')
sloc_crypto=$(cat crypto/*.c crypto/*.h | wc -l | tr -d ' ')
sloc_total=$((sloc_core + sloc_include + sloc_crypto))

echo "## Source Metrics"
echo ""
echo "| Metric | Value |"
echo "|---|---|"
echo "| Core SLOC (src/) | $sloc_core |"
echo "| Public headers (include/) | $sloc_include |"
echo "| Crypto adapters (crypto/) | $sloc_crypto |"
echo "| Total SLOC | $sloc_total |"
echo "| Source files | $(ls src/*.c | wc -l | tr -d ' ') .c + $(ls src/*.h | wc -l | tr -d ' ') .h |"
echo "| External dependencies | 1 ($CRYPTO_NAME) |"
echo ""

# =====================================================================
# Section 4: libdatachannel comparison (optional)
# =====================================================================
if $BUILD_LDC; then
    echo "=== libdatachannel comparison ===" >&2

    ldc_build="$ROOT/.cache/bench-ldc"
    rm -rf "$ldc_build"
    mkdir -p "$ldc_build"

    # Build libdatachannel with minimal config
    cmake -B "$ldc_build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DNANORTC_BUILD_TESTS=OFF \
        -DNANORTC_BUILD_BENCHMARKS=OFF \
        -DFETCHCONTENT_QUIET=ON \
        "$ROOT/tests/interop" > /dev/null 2>&1 || {
        echo "  WARN: libdatachannel build failed, skipping comparison" >&2
    }

    # Try to measure libdatachannel + deps
    ldc_lib="$ldc_build/_deps/libdatachannel-build/libdatachannel-static.a"
    if [ -f "$ldc_lib" ]; then
        ldc_text=$(get_text_size_lib "$ldc_lib")
        echo ""
        echo "## libdatachannel Comparison"
        echo ""
        echo "| Library | .text |"
        echo "|---|---|"
        echo "| libdatachannel (static) | $(format_kb "$ldc_text") |"
        echo "| nanortc (full media) | $(format_kb "${TEXT_SIZES[5]}") |"
        echo ""
    fi
fi

# =====================================================================
# Summary JSON
# =====================================================================
echo "" >&2
echo "--- JSON summary ---" >&2
for i in "${!COMBO_NAMES[@]}"; do
    printf '{"combo":"%s","text_bytes":%s,"sizeof_bytes":%s}\n' \
        "${COMBO_NAMES[$i]}" "${TEXT_SIZES[$i]}" "${RAM_SIZES[$i]}"
done

ARCH=$(uname -m)
OS=$(uname -s)
echo "" >&2
echo "> Measured on ${ARCH} ${OS}, ${CRYPTO_NAME}, -O2 Release." >&2

# Cleanup
rm -rf "$ROOT"/.cache/bench-size-*
