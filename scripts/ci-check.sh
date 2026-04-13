#!/usr/bin/env bash
#
# nanortc — Local CI check script
#
# Runs the same checks as .github/workflows/ci.yml locally.
# Use as pre-push verification:
#   ./scripts/ci-check.sh
#
# Exit code: 0 = all passed, 1 = failures detected

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

CI_DIR="$ROOT/.cache/ci"
mkdir -p "$CI_DIR"

PASS=0
FAIL=0
RESULTS=""

run_check() {
    local name="$1"
    shift
    printf "  %-50s" "$name"
    if "$@" > /dev/null 2>&1; then
        printf " OK\n"
        PASS=$((PASS + 1))
        RESULTS="${RESULTS}\n  OK   $name"
    else
        printf " FAIL\n"
        FAIL=$((FAIL + 1))
        RESULTS="${RESULTS}\n  FAIL $name"
    fi
}

# ============================================================
# 1. Architecture constraints (no build required)
# ============================================================
echo "=== Architecture Constraints ==="

run_check "No platform headers in src/" \
    bash -c '! grep -rn "#include <sys/" src/ && ! grep -rn "#include <pthread" src/ && ! grep -rn "#include <time\.h>" src/ && ! grep -rn "#include <unistd" src/ && ! grep -rn "#include <stdlib\.h>" src/'

run_check "No dynamic allocation in src/" \
    bash -c '! grep -rn "\bmalloc\b" src/ && ! grep -rn "\bcalloc\b" src/ && ! grep -rn "\brealloc\b" src/'

BANNED='strlen|sprintf|snprintf|strcpy|strncpy|strcat|strncat|sscanf|atoi|atol|gets'
run_check "No unbounded string functions in src/+crypto/" \
    bash -c '! grep -rnE "\b('"$BANNED"')\b" src/ crypto/ | grep -v "NANORTC_SAFE"'

run_check "No hardcoded array sizes in struct headers" \
    bash -c '! grep -rnE "\b(uint8_t|char|int8_t|uint16_t|uint32_t)\s+\w+\[\s*[0-9]+\s*\];" src/*.h include/nanortc.h | grep -v "//"'

# ============================================================
# 2. Code formatting
# ============================================================
echo ""
echo "=== Code Formatting ==="

if command -v clang-format > /dev/null 2>&1; then
    run_check "clang-format check" \
        clang-format --dry-run --Werror src/*.c src/*.h include/*.h crypto/*.h crypto/*.c
else
    printf "  %-50s SKIP (clang-format not installed)\n" "clang-format check"
fi

# ============================================================
# 3. Build all feature combinations
# ============================================================
echo ""
echo "=== Feature Combo Builds ==="

# Detect available crypto backends (GH CI tests both)
CRYPTO_BACKENDS=()
if pkg-config --exists openssl 2>/dev/null || [ -f /usr/include/openssl/ssl.h ]; then
    CRYPTO_BACKENDS+=(openssl)
fi
if pkg-config --exists mbedtls 2>/dev/null || [ -f /usr/include/mbedtls/ssl.h ]; then
    CRYPTO_BACKENDS+=(mbedtls)
fi
if [ ${#CRYPTO_BACKENDS[@]} -eq 0 ]; then
    echo "  ERROR: neither openssl nor mbedtls development headers found"
    exit 1
fi
echo "  (crypto backends: ${CRYPTO_BACKENDS[*]})"

# 6 feature combinations (indexed arrays for bash 3.2 compat)
COMBO_NAMES=(  DATA AUDIO MEDIA AUDIO_ONLY MEDIA_ONLY CORE_ONLY )
COMBO_FLAGS=(
    "-DNANORTC_FEATURE_DATACHANNEL=ON  -DNANORTC_FEATURE_AUDIO=OFF -DNANORTC_FEATURE_VIDEO=OFF"
    "-DNANORTC_FEATURE_DATACHANNEL=ON  -DNANORTC_FEATURE_AUDIO=ON  -DNANORTC_FEATURE_VIDEO=OFF"
    "-DNANORTC_FEATURE_DATACHANNEL=ON  -DNANORTC_FEATURE_AUDIO=ON  -DNANORTC_FEATURE_VIDEO=ON"
    "-DNANORTC_FEATURE_DATACHANNEL=OFF -DNANORTC_FEATURE_AUDIO=ON  -DNANORTC_FEATURE_VIDEO=OFF"
    "-DNANORTC_FEATURE_DATACHANNEL=OFF -DNANORTC_FEATURE_AUDIO=ON  -DNANORTC_FEATURE_VIDEO=ON"
    "-DNANORTC_FEATURE_DATACHANNEL=OFF -DNANORTC_FEATURE_AUDIO=OFF -DNANORTC_FEATURE_VIDEO=OFF"
)

for crypto in "${CRYPTO_BACKENDS[@]}"; do
    CRYPTO_FLAG="-DNANORTC_CRYPTO=$crypto"
    for i in "${!COMBO_NAMES[@]}"; do
        combo="${COMBO_NAMES[$i]}"
        flags="${COMBO_FLAGS[$i]}"
        build_dir="$CI_DIR/build-ci-${combo}-${crypto}"
        rm -rf "$build_dir"

        run_check "Build $combo / $crypto" \
            bash -c "cmake -B '$build_dir' $flags $CRYPTO_FLAG -DCMAKE_BUILD_TYPE=Debug > /dev/null 2>&1 && cmake --build '$build_dir' -j\$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4) > /dev/null 2>&1"

        run_check "Test  $combo / $crypto" \
            ctest --test-dir "$build_dir" --output-on-failure
    done
done

# ============================================================
# 4. Symbol checks (using MEDIA build, most complete)
# ============================================================
echo ""
echo "=== Symbol Checks ==="

# Use first available crypto backend for symbol checks
MEDIA_LIB="$CI_DIR/build-ci-MEDIA-${CRYPTO_BACKENDS[0]}/libnanortc.a"
if [ -f "$MEDIA_LIB" ]; then
    # All symbols must use nano_ (public) or known module prefixes (internal)
    ALLOWED='nano_|nanortc_|stun_|ice_|dtls_|nsctp_|sctp_|dc_|sdp_|rtp_|rtcp_|srtp_|jitter_|bwe_|h264_|h265_|media_|ssrc_map_|addr_|track_|turn_'
    run_check "Symbols use allowed prefixes" \
        bash -c 'test -z "$(nm -g '"$MEDIA_LIB"' 2>/dev/null | grep " T " | awk "{print \$3}" | grep -v "^_" | grep -vE "^('"$ALLOWED"')")"'

    run_check "No global mutable state" \
        bash -c 'test -z "$(nm '"$MEDIA_LIB"' 2>/dev/null | grep " [BD] " | grep -v "__" | grep -v "crc32c_table")"'
else
    printf "  %-50s SKIP (MEDIA build failed)\n" "Symbol checks"
fi

# DataChannel-OFF builds should NOT contain nsctp_ symbols
AUDIO_ONLY_LIB="$CI_DIR/build-ci-AUDIO_ONLY-${CRYPTO_BACKENDS[0]}/libnanortc.a"
if [ -f "$AUDIO_ONLY_LIB" ]; then
    run_check "AUDIO_ONLY: no nsctp_ symbols" \
        bash -c 'test -z "$(nm '"$AUDIO_ONLY_LIB"' 2>/dev/null | grep " T " | grep "nsctp_")"'
else
    printf "  %-50s SKIP (AUDIO_ONLY build failed)\n" "AUDIO_ONLY: no nsctp_ symbols"
fi

# ============================================================
# 5. AddressSanitizer build
# ============================================================
echo ""
echo "=== AddressSanitizer ==="

asan_dir="$CI_DIR/build-ci-asan"
rm -rf "$asan_dir"

run_check "Build MEDIA + ASan" \
    bash -c "cmake -B '$asan_dir' -DNANORTC_FEATURE_DATACHANNEL=ON -DNANORTC_FEATURE_AUDIO=ON -DNANORTC_FEATURE_VIDEO=ON $CRYPTO_FLAG -DCMAKE_BUILD_TYPE=Debug -DADDRESS_SANITIZER=ON > /dev/null 2>&1 && cmake --build '$asan_dir' -j\$(nproc) > /dev/null 2>&1"

run_check "Test  MEDIA + ASan" \
    ctest --test-dir "$asan_dir" --output-on-failure

# ============================================================
# 6. Interop tests (requires openssl + C++ compiler)
# ============================================================
echo ""
echo "=== Interop Tests ==="

HAS_OPENSSL=false
for crypto in "${CRYPTO_BACKENDS[@]}"; do
    [ "$crypto" = "openssl" ] && HAS_OPENSSL=true
done

if $HAS_OPENSSL && command -v c++ > /dev/null 2>&1; then
    interop_dir="$CI_DIR/build-ci-interop"
    rm -rf "$interop_dir"

    run_check "Build interop (libdatachannel)" \
        bash -c "cmake -B '$interop_dir' -DNANORTC_BUILD_INTEROP_TESTS=ON -DNANORTC_CRYPTO=openssl -DCMAKE_BUILD_TYPE=Debug -DCMAKE_POLICY_VERSION_MINIMUM=3.5 > /dev/null 2>&1 && cmake --build '$interop_dir' -j\$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4) > /dev/null 2>&1"

    run_check "Test  interop (libdatachannel)" \
        ctest --test-dir "$interop_dir" -R interop --output-on-failure
else
    if ! $HAS_OPENSSL; then
        printf "  %-50s SKIP (openssl not available)\n" "Interop tests"
    else
        printf "  %-50s SKIP (C++ compiler not available)\n" "Interop tests"
    fi
fi

# ============================================================
# Cleanup
# ============================================================
rm -rf "$CI_DIR"/build-ci-*

# ============================================================
# Summary
# ============================================================
echo ""
echo "========================================"
echo "  Results: $PASS passed, $FAIL failed"
echo "========================================"
printf "$RESULTS\n"
echo ""

if [ "$FAIL" -gt 0 ]; then
    echo "CI CHECK FAILED"
    exit 1
else
    echo "CI CHECK PASSED"
    exit 0
fi
