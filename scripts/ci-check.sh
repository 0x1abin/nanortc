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
# 3. Build all profiles
# ============================================================
echo ""
echo "=== Profile Builds ==="

# Auto-detect available crypto backend
if pkg-config --exists openssl 2>/dev/null || [ -f /usr/include/openssl/ssl.h ]; then
    CRYPTO_FLAG="-DNANORTC_CRYPTO=openssl"
    echo "  (using crypto: openssl)"
elif pkg-config --exists mbedtls 2>/dev/null || [ -f /usr/include/mbedtls/ssl.h ]; then
    CRYPTO_FLAG="-DNANORTC_CRYPTO=mbedtls"
    echo "  (using crypto: mbedtls)"
else
    echo "  ERROR: neither openssl nor mbedtls development headers found"
    exit 1
fi

for profile in DATA AUDIO MEDIA; do
    build_dir="$ROOT/build-ci-${profile}"
    rm -rf "$build_dir"

    run_check "Build $profile" \
        bash -c "cmake -B '$build_dir' -DNANORTC_PROFILE=$profile $CRYPTO_FLAG -DCMAKE_BUILD_TYPE=Debug > /dev/null 2>&1 && cmake --build '$build_dir' -j\$(nproc) > /dev/null 2>&1"

    run_check "Test  $profile" \
        ctest --test-dir "$build_dir" --output-on-failure
done

# ============================================================
# 4. Symbol checks (using MEDIA build, most complete)
# ============================================================
echo ""
echo "=== Symbol Checks ==="

MEDIA_LIB="$ROOT/build-ci-MEDIA/libnanortc.a"
if [ -f "$MEDIA_LIB" ]; then
    # All symbols must use nano_ (public) or known module prefixes (internal)
    ALLOWED='nano_|stun_|ice_|dtls_|sctp_|dc_|sdp_|rtp_|rtcp_|srtp_|jitter_|bwe_'
    run_check "Symbols use allowed prefixes" \
        bash -c 'test -z "$(nm -g '"$MEDIA_LIB"' 2>/dev/null | grep " T " | awk "{print \$3}" | grep -v "^_" | grep -vE "^('"$ALLOWED"')")"'

    run_check "No global mutable state" \
        bash -c 'test -z "$(nm '"$MEDIA_LIB"' 2>/dev/null | grep " [BD] " | grep -v "__" | grep -v "crc32c_table")"'
else
    printf "  %-50s SKIP (MEDIA build failed)\n" "Symbol checks"
fi

# ============================================================
# 5. AddressSanitizer build
# ============================================================
echo ""
echo "=== AddressSanitizer ==="

asan_dir="$ROOT/build-ci-asan"
rm -rf "$asan_dir"

run_check "Build MEDIA + ASan" \
    bash -c "cmake -B '$asan_dir' -DNANORTC_PROFILE=MEDIA $CRYPTO_FLAG -DCMAKE_BUILD_TYPE=Debug -DADDRESS_SANITIZER=ON > /dev/null 2>&1 && cmake --build '$asan_dir' -j\$(nproc) > /dev/null 2>&1"

run_check "Test  MEDIA + ASan" \
    ctest --test-dir "$asan_dir" --output-on-failure

# ============================================================
# Cleanup
# ============================================================
rm -rf "$ROOT"/build-ci-*

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
