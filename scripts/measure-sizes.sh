#!/usr/bin/env bash
#
# nanortc — Measure archive code/read-only data and state size for each feature combo
#
# Usage: ./scripts/measure-sizes.sh                    # Host build (auto-detect crypto)
#        ./scripts/measure-sizes.sh --esp32 [TARGET]   # ESP-IDF build (default: esp32p4)
#
# Supported ESP targets: esp32s3, esp32p4, esp32c6, etc.
# Output: Markdown table suitable for README.md

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

ESP32_MODE=false
ESP_TARGET="esp32p4"
if [[ "${1:-}" == "--esp32" ]]; then
    ESP32_MODE=true
    if [ -n "${2:-}" ]; then
        ESP_TARGET="$2"
    fi
fi

NCPU=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# Format bytes as KiB with one decimal place.
format_kb() {
    local bytes="$1"
    if [ "$bytes" = "?" ] || [ -z "$bytes" ]; then
        echo "?"
        return
    fi
    echo "$bytes" | awk '{printf "%d B (%.1f KiB)", $1, $1/1024}'
}

# One profile table drives host defines, CMake options and ESP-IDF defaults.
PROFILES=(
    "CORE_ONLY|Core only|0 0 0 0"
    "DATA|DataChannel|1 0 0 0"
    "AUDIO_ONLY|Audio only|0 1 0 0"
    "AUDIO|DataChannel + Audio|1 1 0 0"
    "MEDIA_ONLY|Media only (no DC)|0 1 1 0"
    "MEDIA|Full media|1 1 1 0"
    "MEDIA_H265|Full media + H.265|1 1 1 1"
)
FEATURES=(DATACHANNEL AUDIO VIDEO H265)
COMBO_NAMES=() COMBO_LABELS=() COMBO_VALUES=() COMBO_SHORT=()
for row in "${PROFILES[@]}"; do
    IFS='|' read -r combo label bits <<< "$row"
    COMBO_NAMES+=("$combo") COMBO_LABELS+=("$label") COMBO_VALUES+=("$bits")
    read -r dc audio video h265 <<< "$bits"
    COMBO_SHORT+=("DC=$dc AUDIO=$audio VIDEO=$video H265=$h265")
done

MEASURE_ROOT="$ROOT/.cache/measure-sizes"
mkdir -p "$MEASURE_ROOT"

run_logged() {
    if ! "$@" >> "$build_log" 2>&1; then
        tail -n 60 "$build_log" >&2
        echo "Build failed; full log: $build_log" >&2
        return 1
    fi
}

# --- ESP-IDF mode ------------------------------------------------------------

if $ESP32_MODE; then
    # Validate environment
    if [ -z "${IDF_PATH:-}" ]; then
        echo "ERROR: IDF_PATH not set. Source esp-idf/export.sh first." >&2
        exit 1
    fi

    MEASURE_DIR="$ROOT/scripts/esp32-measure"
    if [ ! -f "$MEASURE_DIR/CMakeLists.txt" ]; then
        echo "ERROR: $MEASURE_DIR not found" >&2
        exit 1
    fi

    # Auto-detect toolchain prefix based on target architecture
    case "$ESP_TARGET" in
        esp32|esp32s2|esp32s3)
            # Xtensa targets
            TOOL_PREFIX=$(command -v xtensa-esp-elf-size 2>/dev/null | sed 's/-size$//' || true)
            if [ -z "$TOOL_PREFIX" ]; then
                TOOL_PREFIX=$(command -v "xtensa-${ESP_TARGET}-elf-size" 2>/dev/null | sed 's/-size$//' || true)
            fi
            ARCH_LABEL="Xtensa"
            ;;
        *)
            # RISC-V targets (esp32c3, esp32c6, esp32h2, esp32p4, etc.)
            TOOL_PREFIX=$(command -v riscv32-esp-elf-size 2>/dev/null | sed 's/-size$//' || true)
            ARCH_LABEL="RISC-V"
            ;;
    esac

    CROSS_SIZE="${TOOL_PREFIX}-size"
    CROSS_NM="${TOOL_PREFIX}-nm"

    if [ -z "$TOOL_PREFIX" ] || [ ! -x "$CROSS_SIZE" ]; then
        echo "ERROR: ${ARCH_LABEL} toolchain not in PATH. Source esp-idf/export.sh first." >&2
        exit 1
    fi

    # Target-specific chip label for output
    case "$ESP_TARGET" in
        esp32s3) CHIP_LABEL="ESP32-S3 (Xtensa LX7)" ;;
        esp32p4) CHIP_LABEL="ESP32-P4 (RISC-V HP)" ;;
        esp32c6) CHIP_LABEL="ESP32-C6 (RISC-V)" ;;
        esp32c3) CHIP_LABEL="ESP32-C3 (RISC-V)" ;;
        *)       CHIP_LABEL="${ESP_TARGET} (${ARCH_LABEL})" ;;
    esac

    TEXT_SIZES=()
    RAM_SIZES=()

    for i in "${!COMBO_NAMES[@]}"; do
        combo="${COMBO_NAMES[$i]}"
        read -r -a values <<< "${COMBO_VALUES[$i]}"

        echo "Building ${ESP_TARGET}: $combo ..." >&2

        profile_dir="$MEASURE_ROOT/$ESP_TARGET/$combo"
        build_dir="$profile_dir/build"
        mkdir -p "$profile_dir"
        build_log="$profile_dir/build.log"
        : > "$build_log"
        sdkconfig_defaults="$profile_dir/sdkconfig.defaults"
        cp "$MEASURE_DIR/sdkconfig.defaults" "$sdkconfig_defaults"
        for j in "${!FEATURES[@]}"; do
            if [ "${values[$j]}" = 1 ]; then
                echo "CONFIG_NANORTC_FEATURE_${FEATURES[$j]}=y"
            else
                echo "# CONFIG_NANORTC_FEATURE_${FEATURES[$j]} is not set"
            fi
        done >> "$sdkconfig_defaults"
        if [ "${values[0]}" = 1 ]; then
            echo "CONFIG_NANORTC_FEATURE_DC_RELIABLE=y" >> "$sdkconfig_defaults"
            echo "CONFIG_NANORTC_FEATURE_DC_ORDERED=y" >> "$sdkconfig_defaults"
        fi
        # Regenerate only this script's private config; keep build artifacts/logs.
        rm -f "$profile_dir/sdkconfig"
        (
            cd "$MEASURE_DIR"
            run_logged idf.py --no-hints -B "$build_dir" \
                -DIDF_TARGET="$ESP_TARGET" -DSDKCONFIG="$profile_dir/sdkconfig" \
                -DSDKCONFIG_DEFAULTS="$sdkconfig_defaults" build
        )

        # GNU size's text column includes read-only data, before final linking.
        lib="$build_dir/esp-idf/nanortc/libnanortc.a"
        if [ -f "$lib" ]; then
            text_bytes=$("$CROSS_SIZE" "$lib" 2>/dev/null | awk 'NR>1{s+=$1}END{print s}')
            TEXT_SIZES+=("${text_bytes:-?}")
        else
            TEXT_SIZES+=("?")
        fi

        # Read sizeof(nanortc_t) from the ELF
        elf="$build_dir/esp32_measure.elf"
        if [ -f "$elf" ]; then
            # Extract nanortc_sizeof symbol address and read 4 bytes from .rodata
            sizeof_val=$(python3 - "$CROSS_NM" "$CROSS_SIZE" "$elf" <<'PY_ELF'
import subprocess, struct, re, sys

# Get symbol address from nm
nm_tool, size_tool, elf = sys.argv[1:]
nm_out = subprocess.check_output([nm_tool, elf], text=True)
for line in nm_out.splitlines():
    if 'nanortc_sizeof' in line:
        parts = line.split()
        addr = int(parts[0], 16)
        break
else:
    print('?')
    sys.exit(0)

# Find the section containing the symbol via readelf
readelf = size_tool.removesuffix('-size') + '-readelf'
sections_out = subprocess.check_output([readelf, '-SW', elf], text=True)

# Parse sections to find which one contains our address
for line in sections_out.splitlines():
    m = re.search(r'\]\s+(\S+)\s+\S+\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)', line)
    if m:
        sec_name = m.group(1)
        sec_addr = int(m.group(2), 16)
        sec_off  = int(m.group(3), 16)
        sec_size = int(m.group(4), 16)
        if sec_addr <= addr < sec_addr + sec_size:
            file_offset = sec_off + (addr - sec_addr)
            with open(elf, 'rb') as f:
                f.seek(file_offset)
                data = f.read(4)
            val = struct.unpack('<I', data)[0]
            print(val)
            sys.exit(0)

print('?')
PY_ELF
            )
            RAM_SIZES+=("${sizeof_val:-?}")
        else
            RAM_SIZES+=("?")
        fi
    done

    # Output markdown table
    echo ""
    echo "| Configuration | Archive code + read-only data | State (sizeof) | Flags |"
    echo "|--------------|---------------|-------------|-------|"

    for i in "${!COMBO_NAMES[@]}"; do
        label="${COMBO_LABELS[$i]}"
        text=$(format_kb "${TEXT_SIZES[$i]}")
        ram=$(format_kb "${RAM_SIZES[$i]}")
        flags="${COMBO_SHORT[$i]}"
        echo "| $label | $text | $ram | $flags |"
    done

    echo ""
    echo "> Measured on ${CHIP_LABEL}, mbedTLS adapter, -Os; excludes the mbedTLS library and final-link garbage collection."
    echo "> \`sizeof(nanortc_t)\` excludes crypto-provider allocations, application buffers and task stacks."

    echo "Build artifacts and logs: $MEASURE_ROOT/$ESP_TARGET" >&2

    exit 0
fi

# --- Host mode ----------------------------------------------------------------

# Auto-detect crypto backend
if pkg-config --exists openssl 2>/dev/null || [ -f /usr/include/openssl/ssl.h ]; then
    CRYPTO_FLAG="-DNANORTC_CRYPTO=openssl"
    CRYPTO_NAME="OpenSSL"
elif pkg-config --exists mbedtls 2>/dev/null || [ -f /usr/include/mbedtls/ssl.h ]; then
    CRYPTO_FLAG="-DNANORTC_CRYPTO=mbedtls"
    CRYPTO_NAME="mbedtls"
else
    echo "ERROR: neither openssl nor mbedtls development headers found" >&2
    exit 1
fi

IS_MACOS=false
if [[ "$(uname)" == "Darwin" ]]; then
    IS_MACOS=true
fi

# Get .text size from a static library
get_text_size() {
    local lib="$1"
    if $IS_MACOS; then
        # macOS: sum __text section sizes across all .o in the archive
        size -m "$lib" 2>/dev/null | grep '__TEXT, __text' | awk -F: '{s+=$2}END{print s}'
    else
        # Linux: size outputs text column; sum across all .o in archive
        size "$lib" 2>/dev/null | awk 'NR>1{s+=$1}END{print s}'
    fi
}

TEXT_SIZES=()
RAM_SIZES=()

for i in "${!COMBO_NAMES[@]}"; do
    combo="${COMBO_NAMES[$i]}"
    read -r -a values <<< "${COMBO_VALUES[$i]}"
    cmake_flags=()
    for j in "${!FEATURES[@]}"; do
        cmake_flags+=("-DNANORTC_FEATURE_${FEATURES[$j]}=${values[$j]}")
    done
    build_dir="$MEASURE_ROOT/host-$CRYPTO_NAME/$combo"
    mkdir -p "$build_dir"
    build_log="$build_dir/build.log"
    : > "$build_log"

    echo "Building $combo ..." >&2
    run_logged cmake -B "$build_dir" "${cmake_flags[@]}" "$CRYPTO_FLAG" -DCMAKE_BUILD_TYPE=Release
    run_logged cmake --build "$build_dir" -j"$NCPU"

    # Measure .text size
    lib="$build_dir/libnanortc.a"
    if [ -f "$lib" ]; then
        text_bytes=$(get_text_size "$lib")
        TEXT_SIZES+=("${text_bytes:-?}")
    else
        TEXT_SIZES+=("?")
    fi

    # Measure sizeof(nanortc_t)
    sizeof_prog="$build_dir/_sizeof_nanortc.c"
    cat > "$sizeof_prog" << 'SIZEOF_EOF'
#include <stdio.h>
#include "nanortc.h"
int main(void) {
    printf("%zu\n", sizeof(nanortc_t));
    return 0;
}
SIZEOF_EOF

    sizeof_bin="$build_dir/_sizeof_nanortc"
    run_logged cc -I"$ROOT/include" -I"$ROOT/src" -I"$ROOT/crypto" "${cmake_flags[@]}" \
        "$sizeof_prog" -o "$sizeof_bin"

    if [ -x "$sizeof_bin" ]; then
        RAM_SIZES+=("$("$sizeof_bin")")
    else
        RAM_SIZES+=("?")
    fi
done

# Output markdown table
echo ""
echo "| Configuration | Archive code / read-only data (see below) | State (sizeof) | Flags |"
echo "|--------------|---------------|-------------|-------|"

for i in "${!COMBO_NAMES[@]}"; do
    label="${COMBO_LABELS[$i]}"
    text=$(format_kb "${TEXT_SIZES[$i]}")
    ram=$(format_kb "${RAM_SIZES[$i]}")
    flags="${COMBO_SHORT[$i]}"
    echo "| $label | $text | $ram | $flags |"
done

echo ""
ARCH=$(uname -m)
OS=$(uname -s)
echo "> Measured on ${ARCH} ${OS}, ${CRYPTO_NAME}, CMake Release. ARM Cortex-M sizes differ (smaller pointers, different alignment)."
echo "> GNU size includes read-only data; macOS reports __text. Crypto libraries and final-link garbage collection are excluded."
echo "> sizeof(nanortc_t) excludes crypto-provider allocations, application buffers and task stacks."

echo "Build artifacts and logs: $MEASURE_ROOT/host-$CRYPTO_NAME" >&2
