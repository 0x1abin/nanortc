#!/bin/bash
# coverage.sh — Build with coverage, run tests, generate report
#
# Usage:
#   ./scripts/coverage.sh              # Full report (MEDIA profile, mbedtls)
#   ./scripts/coverage.sh --threshold 80  # Fail if line coverage < 80%
#   ./scripts/coverage.sh --open       # Open HTML report in browser
#
# Output:
#   build-cov/coverage/          — HTML report
#   build-cov/coverage.info      — lcov data
#   build-cov/coverage-summary.txt — per-file summary
#
# SPDX-License-Identifier: MIT

set -euo pipefail

THRESHOLD=0
OPEN_REPORT=false
BUILD_DIR="build-cov"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --threshold) THRESHOLD="$2"; shift 2 ;;
        --open) OPEN_REPORT=true; shift ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

cd "$PROJECT_DIR"

echo "=== nanortc code coverage ==="

# Build with coverage
echo "--- Configure ---"
cmake -B "$BUILD_DIR" \
    -DNANORTC_COVERAGE=ON \
    -DNANORTC_FEATURE_DATACHANNEL=ON \
    -DNANORTC_FEATURE_AUDIO=ON \
    -DNANORTC_FEATURE_VIDEO=ON \
    -DNANORTC_FEATURE_IPV6=ON \
    -DCMAKE_BUILD_TYPE=Debug \
    2>&1 | tail -3

echo "--- Build ---"
cmake --build "$BUILD_DIR" -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)" 2>&1 | tail -3

# Clean previous coverage data
find "$BUILD_DIR" -name '*.gcda' -delete 2>/dev/null || true

# Run tests
echo "--- Run tests ---"
ctest --test-dir "$BUILD_DIR" --output-on-failure 2>&1 | tail -5

# Capture coverage
echo "--- Capture coverage ---"
lcov --capture \
    --directory "$BUILD_DIR" \
    --output-file "$BUILD_DIR/coverage-raw.info" \
    --ignore-errors inconsistent,inconsistent \
    --quiet

# Filter to only src/ and crypto/ (exclude tests, third_party, system headers)
lcov --extract "$BUILD_DIR/coverage-raw.info" \
    "*/src/*.c" "*/crypto/*.c" \
    --output-file "$BUILD_DIR/coverage.info" \
    --ignore-errors unused,unused \
    --quiet

# Generate HTML report
echo "--- Generate report ---"
genhtml "$BUILD_DIR/coverage.info" \
    --output-directory "$BUILD_DIR/coverage" \
    --title "nanortc coverage" \
    --quiet

# Per-file summary
echo ""
echo "=== Per-file line coverage ==="
lcov --summary "$BUILD_DIR/coverage.info" 2>&1 | grep -E "lines|functions"
echo ""

# Detailed per-file breakdown
lcov --list "$BUILD_DIR/coverage.info" 2>&1 | tee "$BUILD_DIR/coverage-summary.txt"

# Extract overall line coverage percentage
LINE_COV=$(lcov --summary "$BUILD_DIR/coverage.info" 2>&1 | grep "lines" | grep -oE '[0-9]+\.[0-9]+' | head -1)
echo ""
echo "Overall line coverage: ${LINE_COV}%"
echo "HTML report: $BUILD_DIR/coverage/index.html"

# Threshold check
if [ "$THRESHOLD" -gt 0 ]; then
    LINE_INT=${LINE_COV%.*}
    if [ "$LINE_INT" -lt "$THRESHOLD" ]; then
        echo "FAIL: Line coverage ${LINE_COV}% < ${THRESHOLD}% threshold"
        exit 1
    else
        echo "PASS: Line coverage ${LINE_COV}% >= ${THRESHOLD}% threshold"
    fi
fi

# Open report if requested
if [ "$OPEN_REPORT" = true ]; then
    open "$BUILD_DIR/coverage/index.html" 2>/dev/null || \
    xdg-open "$BUILD_DIR/coverage/index.html" 2>/dev/null || \
    echo "Open $BUILD_DIR/coverage/index.html manually"
fi
