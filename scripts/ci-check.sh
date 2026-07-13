#!/usr/bin/env bash
#
# nanortc — Local CI check script
#
# Runs the same checks as .github/workflows/ci.yml locally.
# Use as pre-push verification:
#   ./scripts/ci-check.sh           # full matrix (mirrors GitHub CI)
#   ./scripts/ci-check.sh --fast    # tier-1 subset for tight loops
#   ./scripts/ci-check.sh --clean   # wipe build dirs before configure
#
# Exit code: 0 = all passed, 1 = failures detected
#
# ----------------------------------------------------------------
# Speed knobs (these are the difference between an 8-minute pre-push
# and a 30-second pre-push; see docs/engineering/development-workflow.md):
#
#   1. ccache — auto-detected. When present we wire it in as
#      CMAKE_C_COMPILER_LAUNCHER, which cuts compile time ~5-10× on
#      repeat builds. To inspect: `ccache -s`.
#
#   2. Incremental cmake — by default we DO NOT `rm -rf` build
#      directories between runs. cmake reuses CMakeCache.txt and
#      ninja/make incremental graphs, so unchanged TUs are skipped.
#      Pass `--clean` to force a full rebuild.
#
#   3. `--fast` mode — runs only the tier that catches ~90% of
#      regressions: arch checks + clang-format + DATA + MEDIA +
#      ASan. Skips the AUDIO_ONLY/MEDIA_ONLY/CORE_ONLY combos and
#      the slow libdatachannel interop suite. Use this in your tight
#      pre-push loop; do a full run before pushing release branches.
# ----------------------------------------------------------------

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

CI_DIR="$ROOT/.cache/ci"
mkdir -p "$CI_DIR"
LOG_DIR="$CI_DIR/logs"
mkdir -p "$LOG_DIR"

# ---- Argument parsing --------------------------------------------------
FAST_MODE=0
CLEAN_MODE=0
for arg in "$@"; do
    case "$arg" in
        --fast)  FAST_MODE=1 ;;
        --clean) CLEAN_MODE=1 ;;
        -h|--help)
            sed -n '2,32p' "$0"
            exit 0
            ;;
        *)
            echo "Unknown flag: $arg" >&2
            echo "Run with --help for usage." >&2
            exit 2
            ;;
    esac
done

# ---- ccache detection --------------------------------------------------
LAUNCHER_FLAGS=""
if command -v ccache > /dev/null 2>&1; then
    LAUNCHER_FLAGS="-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
    echo "  (ccache: $(ccache --version | head -1))"
else
    echo "  (ccache: not installed — install via 'brew install ccache' for ~5-10× speedup)"
fi

if [ $FAST_MODE -eq 1 ]; then
    echo "  (mode: --fast — skipping AUDIO_ONLY/MEDIA_ONLY/CORE_ONLY combos and libdatachannel interop)"
fi
if [ $CLEAN_MODE -eq 1 ]; then
    echo "  (mode: --clean — wiping build dirs before reconfigure)"
fi

# Helper: prepare a build directory. With --clean we always rm -rf;
# without --clean we keep the dir so cmake can reuse its cache and the
# compile cache (ccache or otherwise) can hit on identical TUs.
prep_build_dir() {
    local dir="$1"
    if [ $CLEAN_MODE -eq 1 ]; then
        rm -rf "$dir"
    fi
    mkdir -p "$dir"
}

PASS=0
FAIL=0
RESULTS=""

run_check() {
    local name="$1"
    shift
    local slug
    local log_file
    slug=$(printf "%s" "$name" | sed 's/[^A-Za-z0-9._-]/_/g')
    log_file="$LOG_DIR/$slug.log"
    printf "  %-50s" "$name"
    if "$@" >"$log_file" 2>&1; then
        printf " OK\n"
        PASS=$((PASS + 1))
        RESULTS="${RESULTS}\n  OK   $name"
    else
        printf " FAIL\n"
        echo "  --- failure log: $log_file ---"
        tail -100 "$log_file"
        echo "  --- end failure log ---"
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
PRIMARY_CRYPTO="${CRYPTO_BACKENDS[0]}"
PRIMARY_CRYPTO_FLAG="-DNANORTC_CRYPTO=$PRIMARY_CRYPTO"
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

invalid_crypto_dir="$CI_DIR/build-ci-invalid-crypto"
prep_build_dir "$invalid_crypto_dir"
run_check "Reject invalid crypto backend" \
    bash -c "! cmake -B '$invalid_crypto_dir' -DNANORTC_CRYPTO=invalid -DNANORTC_BUILD_TESTS=OFF"

invalid_tx_slots_dir="$CI_DIR/build-ci-invalid-tx-slots"
prep_build_dir "$invalid_tx_slots_dir"
run_check "Reject invalid TX slot count" \
    bash -c "! cmake -B '$invalid_tx_slots_dir' -DNANORTC_TX_SLOT_COUNT=3 -DNANORTC_BUILD_TESTS=OFF"

tx_slot_one_dir="$CI_DIR/build-ci-tx-slot-one"
prep_build_dir "$tx_slot_one_dir"
run_check "Build DATA + TX_SLOT_COUNT=1" \
    bash -c "cmake -B '$tx_slot_one_dir' -DNANORTC_TX_SLOT_COUNT=1 $PRIMARY_CRYPTO_FLAG $LAUNCHER_FLAGS -DCMAKE_BUILD_TYPE=Debug && cmake --build '$tx_slot_one_dir' -j${JOBS}"
run_check "Test  DATA + TX_SLOT_COUNT=1" \
    ctest --test-dir "$tx_slot_one_dir" --output-on-failure

idf_env_host_dir="$CI_DIR/build-ci-idf-env-host"
prep_build_dir "$idf_env_host_dir"
run_check "IDF_PATH alone keeps host CMake mode" \
    env IDF_PATH=/nonexistent cmake -B "$idf_env_host_dir" \
        -DNANORTC_BUILD_TESTS=OFF "$PRIMARY_CRYPTO_FLAG" -DCMAKE_BUILD_TYPE=Debug

# 7 feature combinations (indexed arrays for bash 3.2 compat).
# MEDIA_H265 covers the H.265 sub-feature explicitly since H.265 is opt-in
# (NANORTC_FEATURE_H265 defaults to OFF, even when VIDEO=ON).
COMBO_NAMES=(  DATA AUDIO MEDIA MEDIA_H265 AUDIO_ONLY MEDIA_ONLY CORE_ONLY )
COMBO_FLAGS=(
    "-DNANORTC_FEATURE_DATACHANNEL=ON  -DNANORTC_FEATURE_AUDIO=OFF -DNANORTC_FEATURE_VIDEO=OFF"
    "-DNANORTC_FEATURE_DATACHANNEL=ON  -DNANORTC_FEATURE_AUDIO=ON  -DNANORTC_FEATURE_VIDEO=OFF"
    "-DNANORTC_FEATURE_DATACHANNEL=ON  -DNANORTC_FEATURE_AUDIO=ON  -DNANORTC_FEATURE_VIDEO=ON"
    "-DNANORTC_FEATURE_DATACHANNEL=ON  -DNANORTC_FEATURE_AUDIO=ON  -DNANORTC_FEATURE_VIDEO=ON  -DNANORTC_FEATURE_H265=ON"
    "-DNANORTC_FEATURE_DATACHANNEL=OFF -DNANORTC_FEATURE_AUDIO=ON  -DNANORTC_FEATURE_VIDEO=OFF"
    "-DNANORTC_FEATURE_DATACHANNEL=OFF -DNANORTC_FEATURE_AUDIO=ON  -DNANORTC_FEATURE_VIDEO=ON"
    "-DNANORTC_FEATURE_DATACHANNEL=OFF -DNANORTC_FEATURE_AUDIO=OFF -DNANORTC_FEATURE_VIDEO=OFF"
)

# In --fast mode, only the most representative combos are built. The skipped
# combos are exotic (non-default flag intersections) and rarely catch bugs
# that DATA / MEDIA don't already surface.
FAST_COMBO_FILTER=" DATA MEDIA "

for crypto in "${CRYPTO_BACKENDS[@]}"; do
    CRYPTO_FLAG="-DNANORTC_CRYPTO=$crypto"
    # In --fast mode, only exercise the first available crypto backend.
    if [ $FAST_MODE -eq 1 ] && [ "$crypto" != "${CRYPTO_BACKENDS[0]}" ]; then
        continue
    fi
    for i in "${!COMBO_NAMES[@]}"; do
        combo="${COMBO_NAMES[$i]}"
        flags="${COMBO_FLAGS[$i]}"

        if [ $FAST_MODE -eq 1 ] && [[ "$FAST_COMBO_FILTER" != *" $combo "* ]]; then
            continue
        fi

        build_dir="$CI_DIR/build-ci-${combo}-${crypto}"
        prep_build_dir "$build_dir"

        run_check "Build $combo / $crypto" \
            bash -c "cmake -B '$build_dir' $flags $CRYPTO_FLAG $LAUNCHER_FLAGS -DCMAKE_BUILD_TYPE=Debug && cmake --build '$build_dir' -j${JOBS}"

        run_check "Test  $combo / $crypto" \
            ctest --test-dir "$build_dir" --output-on-failure
    done
done

# ============================================================
# 4. Symbol checks (using MEDIA build, most complete)
# ============================================================
echo ""
echo "=== Symbol Checks ==="

# Symbol checks read libnanortc.a from a *previous* build dir. In --fast
# mode we skip the AUDIO_ONLY combo entirely, so its .a is stale (or
# missing); reading it would either lie or silently skip. Force-skip the
# AUDIO_ONLY-derived check in --fast mode rather than risk a misleading
# pass on out-of-date binaries.
MEDIA_LIB="$CI_DIR/build-ci-MEDIA-${CRYPTO_BACKENDS[0]}/libnanortc.a"
if [ -f "$MEDIA_LIB" ]; then
    # All symbols must use nano_ (public) or known module prefixes (internal)
    ALLOWED='nano_|nanortc_|stun_|ice_|dtls_|nsctp_|sctp_|dc_|sdp_|rtp_|rtcp_|srtp_|jitter_|bwe_|twcc_|rate_window_|rate_control_|h264_|h265_|media_|ssrc_map_|addr_|track_|turn_'
    run_check "Symbols use allowed prefixes" \
        bash -c 'test -z "$(nm -g '"$MEDIA_LIB"' 2>/dev/null | grep " T " | awk "{print \$3}" | grep -v "^_" | grep -vE "^('"$ALLOWED"')")"'

    run_check "No global mutable state" \
        bash -c 'test -z "$(nm '"$MEDIA_LIB"' 2>/dev/null | grep " [BD] " | grep -v "__" | grep -v "crc32c_table")"'
else
    printf "  %-50s SKIP (MEDIA build failed)\n" "Symbol checks"
fi

# DataChannel-OFF builds should NOT contain nsctp_ symbols. Skip in --fast
# mode where AUDIO_ONLY is intentionally not rebuilt.
if [ $FAST_MODE -eq 1 ]; then
    printf "  %-50s SKIP (--fast mode skips AUDIO_ONLY rebuild)\n" "AUDIO_ONLY: no nsctp_ symbols"
else
    AUDIO_ONLY_LIB="$CI_DIR/build-ci-AUDIO_ONLY-${CRYPTO_BACKENDS[0]}/libnanortc.a"
    if [ -f "$AUDIO_ONLY_LIB" ]; then
        run_check "AUDIO_ONLY: no nsctp_ symbols" \
            bash -c 'test -z "$(nm '"$AUDIO_ONLY_LIB"' 2>/dev/null | grep " T " | grep "nsctp_")"'
    else
        printf "  %-50s SKIP (AUDIO_ONLY build failed)\n" "AUDIO_ONLY: no nsctp_ symbols"
    fi
fi

# ============================================================
# 5. AddressSanitizer build
# ============================================================
echo ""
echo "=== AddressSanitizer ==="

asan_dir="$CI_DIR/build-ci-asan"
prep_build_dir "$asan_dir"

run_check "Build all advanced media + H265 + ASan" \
    bash -c "cmake -B '$asan_dir' -DNANORTC_FEATURE_DATACHANNEL=ON -DNANORTC_FEATURE_AUDIO=ON -DNANORTC_FEATURE_VIDEO=ON -DNANORTC_FEATURE_H265=ON -DNANORTC_FEATURE_VIDEO_RATE_CONTROL=ON -DNANORTC_FEATURE_VIDEO_REORDER=ON -DNANORTC_FEATURE_VIDEO_NACK_RX=ON -DNANORTC_FEATURE_VIDEO_FEC=ON -DNANORTC_FEATURE_VIDEO_PACING=ON -DNANORTC_FEATURE_VIDEO_AUTO_PLI=ON -DNANORTC_FEC_ADAPTIVE=ON $PRIMARY_CRYPTO_FLAG $LAUNCHER_FLAGS -DCMAKE_BUILD_TYPE=Debug -DADDRESS_SANITIZER=ON && cmake --build '$asan_dir' -j${JOBS}"

run_check "Test  all advanced media + H265 + ASan" \
    ctest --test-dir "$asan_dir" --output-on-failure

# ============================================================
# 5b. Feature-OFF builds (catch dead code / leaked refs)
# ============================================================
# ICE_SRFLX is the only feature flag added recently that gates code outside
# its own module — verify the OFF path still compiles and tests pass.
echo ""
echo "=== Feature-OFF Builds ==="

srflx_off_dir="$CI_DIR/build-ci-srflx-off"
prep_build_dir "$srflx_off_dir"

run_check "Build DATA + ICE_SRFLX=OFF" \
    bash -c "cmake -B '$srflx_off_dir' -DNANORTC_FEATURE_DATACHANNEL=ON -DNANORTC_FEATURE_AUDIO=OFF -DNANORTC_FEATURE_VIDEO=OFF -DNANORTC_FEATURE_ICE_SRFLX=OFF $PRIMARY_CRYPTO_FLAG $LAUNCHER_FLAGS -DCMAKE_BUILD_TYPE=Debug && cmake --build '$srflx_off_dir' -j${JOBS}"

run_check "Test  DATA + ICE_SRFLX=OFF" \
    ctest --test-dir "$srflx_off_dir" --output-on-failure

# Feature-ON: the opt-in receive-robustness features (reorder buffer + receiver
# NACK, both default off) — the default matrix never compiles nano_reorder.c,
# the reorder/NACK receive paths, or test_reorder, so exercise them together
# (the natural pairing: reorder holds the gap, NACK requests the retransmit).
reorder_dir="$CI_DIR/build-ci-reorder"
prep_build_dir "$reorder_dir"

run_check "Build MEDIA + REORDER + NACK_RX + FEC + RATE_CONTROL" \
    bash -c "cmake -B '$reorder_dir' -DNANORTC_FEATURE_DATACHANNEL=ON -DNANORTC_FEATURE_AUDIO=ON -DNANORTC_FEATURE_VIDEO=ON -DNANORTC_FEATURE_VIDEO_REORDER=ON -DNANORTC_FEATURE_VIDEO_NACK_RX=ON -DNANORTC_FEATURE_VIDEO_FEC=ON -DNANORTC_FEATURE_VIDEO_RATE_CONTROL=ON $PRIMARY_CRYPTO_FLAG $LAUNCHER_FLAGS -DCMAKE_BUILD_TYPE=Debug && cmake --build '$reorder_dir' -j${JOBS}"

run_check "Test  MEDIA + REORDER + NACK_RX + FEC + RATE_CONTROL" \
    ctest --test-dir "$reorder_dir" --output-on-failure

fec_no_nack_dir="$CI_DIR/build-ci-fec-no-nack"
prep_build_dir "$fec_no_nack_dir"

run_check "Build MEDIA + FEC + NACK_RX=OFF + AUTO_PLI=OFF" \
    bash -c "cmake -B '$fec_no_nack_dir' -DNANORTC_FEATURE_DATACHANNEL=ON -DNANORTC_FEATURE_AUDIO=ON -DNANORTC_FEATURE_VIDEO=ON -DNANORTC_FEATURE_H265=ON -DNANORTC_FEATURE_VIDEO_REORDER=ON -DNANORTC_FEATURE_VIDEO_FEC=ON -DNANORTC_FEATURE_VIDEO_NACK_RX=OFF -DNANORTC_FEC_ADAPTIVE=ON -DNANORTC_FEATURE_VIDEO_PACING=ON -DNANORTC_FEATURE_VIDEO_AUTO_PLI=OFF $PRIMARY_CRYPTO_FLAG $LAUNCHER_FLAGS -DCMAKE_BUILD_TYPE=Debug && cmake --build '$fec_no_nack_dir' -j${JOBS}"

run_check "Test  MEDIA + FEC + NACK_RX=OFF + AUTO_PLI=OFF" \
    ctest --test-dir "$fec_no_nack_dir" --output-on-failure

pacing_off_dir="$CI_DIR/build-ci-pacing-off"
prep_build_dir "$pacing_off_dir"

run_check "Build MEDIA + FEC + PACING=OFF + ADAPTIVE=OFF" \
    bash -c "cmake -B '$pacing_off_dir' -DNANORTC_FEATURE_DATACHANNEL=ON -DNANORTC_FEATURE_AUDIO=ON -DNANORTC_FEATURE_VIDEO=ON -DNANORTC_FEATURE_H265=ON -DNANORTC_FEATURE_VIDEO_REORDER=ON -DNANORTC_FEATURE_VIDEO_NACK_RX=ON -DNANORTC_FEATURE_VIDEO_FEC=ON -DNANORTC_FEATURE_VIDEO_PACING=OFF -DNANORTC_FEATURE_VIDEO_AUTO_PLI=ON -DNANORTC_FEC_ADAPTIVE=OFF $PRIMARY_CRYPTO_FLAG $LAUNCHER_FLAGS -DCMAKE_BUILD_TYPE=Debug && cmake --build '$pacing_off_dir' -j${JOBS}"

run_check "Test  MEDIA + FEC + PACING=OFF + ADAPTIVE=OFF" \
    ctest --test-dir "$pacing_off_dir" --output-on-failure

# ============================================================
# 6. Interop tests (requires openssl + C++ compiler)
# ============================================================
echo ""
echo "=== Interop Tests ==="

HAS_OPENSSL=false
for crypto in "${CRYPTO_BACKENDS[@]}"; do
    [ "$crypto" = "openssl" ] && HAS_OPENSSL=true
done

if [ $FAST_MODE -eq 1 ]; then
    printf "  %-50s SKIP (--fast mode)\n" "Interop tests"
elif $HAS_OPENSSL && command -v c++ > /dev/null 2>&1; then
    interop_dir="$CI_DIR/build-ci-interop"
    prep_build_dir "$interop_dir"

    run_check "Build interop (libdatachannel)" \
        bash -c "cmake -B '$interop_dir' -DNANORTC_BUILD_INTEROP_TESTS=ON -DNANORTC_CRYPTO=openssl -DNANORTC_FEATURE_AUDIO=ON -DNANORTC_FEATURE_VIDEO=ON -DNANORTC_FEATURE_H265=ON $LAUNCHER_FLAGS -DCMAKE_BUILD_TYPE=Debug -DCMAKE_POLICY_VERSION_MINIMUM=3.5 && cmake --build '$interop_dir' -j${JOBS}"

    run_check "Assert A/V interop tests registered" \
        bash -c "tests=\$(ctest --test-dir '$interop_dir' -N); printf '%s\n' \"\$tests\"; grep -q 'interop_audio' <<<\"\$tests\" && grep -q 'interop_video' <<<\"\$tests\""

    run_check "Test  interop (libdatachannel)" \
        ctest --test-dir "$interop_dir" -R interop -LE network --output-on-failure
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
# Build dirs are persistent across runs by default — ccache + cmake
# incremental graphs need them. Pass --clean to wipe them next run, or
# simply `rm -rf .cache/ci/build-ci-*` manually.

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
