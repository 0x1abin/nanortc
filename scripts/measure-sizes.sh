#!/usr/bin/env bash
#
# nanortc — Measure Flash (.text) and RAM (sizeof) for each feature combo
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

# Format bytes to KB (round to nearest integer)
format_kb() {
    local bytes="$1"
    if [ "$bytes" = "?" ] || [ -z "$bytes" ]; then
        echo "?"
        return
    fi
    echo "$bytes" | awk '{printf "%.1f KB", $1/1024}'
}

# Arrays indexed by position (bash 3.2 compatible)
COMBO_NAMES=(  CORE_ONLY    DATA    AUDIO_ONLY    AUDIO    MEDIA_ONLY    MEDIA)
COMBO_LABELS=( "Core only"  "DataChannel"  "Audio only"  "DataChannel + Audio"  "Media only (no DC)"  "Full media")
COMBO_SHORT=(
    "DC=OFF AUDIO=OFF VIDEO=OFF"
    "DC=ON"
    "DC=OFF AUDIO=ON"
    "DC=ON AUDIO=ON"
    "DC=OFF AUDIO=ON VIDEO=ON"
    "DC=ON AUDIO=ON VIDEO=ON"
)

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

    # Kconfig feature configs for each combo (DC, AUDIO, VIDEO)
    COMBO_KCONFIG=(
        "n n n"
        "y n n"
        "n y n"
        "y y n"
        "n y y"
        "y y y"
    )

    # Auto-detect toolchain prefix based on target architecture
    case "$ESP_TARGET" in
        esp32|esp32s2|esp32s3)
            # Xtensa targets
            TOOL_PREFIX=$(command -v xtensa-esp-elf-size 2>/dev/null | sed 's/-size$//' || true)
            if [ -z "$TOOL_PREFIX" ]; then
                TOOL_PREFIX=$(command -v "xtensa-${ESP_TARGET/esp32/esp32s3}-elf-size" 2>/dev/null | sed 's/-size$//' || true)
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
        read -r dc audio video <<< "${COMBO_KCONFIG[$i]}"

        echo "Building ${ESP_TARGET}: $combo ..." >&2

        build_dir="$MEASURE_DIR/build-${combo}"

        # Write per-combo sdkconfig.defaults with feature flags
        sdkconfig_defaults="$MEASURE_DIR/sdkconfig.defaults.combo"
        cp "$MEASURE_DIR/sdkconfig.defaults" "$sdkconfig_defaults"
        {
            echo ""
            echo "# Feature flags for $combo"
            if [ "$dc" = "y" ]; then
                echo "CONFIG_NANORTC_FEATURE_DATACHANNEL=y"
                echo "CONFIG_NANORTC_FEATURE_DC_RELIABLE=y"
                echo "CONFIG_NANORTC_FEATURE_DC_ORDERED=y"
            else
                echo "# CONFIG_NANORTC_FEATURE_DATACHANNEL is not set"
            fi
            if [ "$audio" = "y" ]; then
                echo "CONFIG_NANORTC_FEATURE_AUDIO=y"
            else
                echo "# CONFIG_NANORTC_FEATURE_AUDIO is not set"
            fi
            if [ "$video" = "y" ]; then
                echo "CONFIG_NANORTC_FEATURE_VIDEO=y"
            else
                echo "# CONFIG_NANORTC_FEATURE_VIDEO is not set"
            fi
        } >> "$sdkconfig_defaults"

        # Clean previous build to ensure feature flags take effect
        rm -rf "$build_dir" "$MEASURE_DIR/sdkconfig"

        # Build with idf.py
        (
            cd "$MEASURE_DIR"
            idf.py -B "build-${combo}" \
                -DSDKCONFIG_DEFAULTS="$sdkconfig_defaults" \
                set-target "$ESP_TARGET" > /dev/null 2>&1
            idf.py -B "build-${combo}" build > /dev/null 2>&1
        )

        # Measure .text from libnanortc.a
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
            sizeof_val=$( python3 -c "
import subprocess, struct, re, sys

# Get symbol address from nm
nm_out = subprocess.check_output(['$CROSS_NM', '$elf'], text=True)
for line in nm_out.splitlines():
    if 'nanortc_sizeof' in line:
        parts = line.split()
        addr = int(parts[0], 16)
        break
else:
    print('?')
    sys.exit(0)

# Find the section containing the symbol via readelf
readelf = '$CROSS_SIZE'.replace('-size', '-readelf')
sections_out = subprocess.check_output([readelf, '-S', '$elf'], text=True)

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
            with open('$elf', 'rb') as f:
                f.seek(file_offset)
                data = f.read(4)
            val = struct.unpack('<I', data)[0]
            print(val)
            sys.exit(0)

print('?')
" 2>/dev/null )
            RAM_SIZES+=("${sizeof_val:-?}")
        else
            RAM_SIZES+=("?")
        fi
    done

    # Output markdown table
    echo ""
    echo "| Configuration | Flash (.text) | RAM (sizeof) | Flags |"
    echo "|--------------|---------------|-------------|-------|"

    for i in "${!COMBO_NAMES[@]}"; do
        label="${COMBO_LABELS[$i]}"
        text=$(format_kb "${TEXT_SIZES[$i]}")
        ram=$(format_kb "${RAM_SIZES[$i]}")
        flags="${COMBO_SHORT[$i]}"
        echo "| $label | $text | $ram | $flags |"
    done

    echo ""
    echo "> Measured on ${CHIP_LABEL}, mbedTLS, -O2."
    echo "> \`sizeof(nanortc_t)\` is the full per-connection RAM — no heap allocation."

    # Cleanup
    rm -rf "$MEASURE_DIR"/build-* "$MEASURE_DIR/sdkconfig" "$MEASURE_DIR/sdkconfig.defaults.combo"

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
    build_dir="$ROOT/build-measure-${combo}"
    rm -rf "$build_dir"

    echo "Building $combo ..." >&2
    eval cmake -B "$build_dir" $cmake_flags $CRYPTO_FLAG \
          -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1
    cmake --build "$build_dir" -j"$NCPU" > /dev/null 2>&1

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

    # Convert cmake ON/OFF to cc -D...=1/0
    defines=""
    for flag in $cmake_flags; do
        define=$(echo "$flag" | sed 's/=ON$/=1/' | sed 's/=OFF$/=0/')
        defines="$defines $define"
    done

    sizeof_bin="$build_dir/_sizeof_nanortc"
    cc -I"$ROOT/include" -I"$ROOT/src" -I"$ROOT/crypto" $defines \
       "$sizeof_prog" -o "$sizeof_bin" 2>/dev/null || true

    if [ -x "$sizeof_bin" ]; then
        RAM_SIZES+=("$("$sizeof_bin")")
    else
        RAM_SIZES+=("?")
    fi
done

# Output markdown table
echo ""
echo "| Configuration | Flash (.text) | RAM (sizeof) | Flags |"
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
echo "> Measured on ${ARCH} ${OS}, ${CRYPTO_NAME}, -O2. ARM Cortex-M sizes differ (smaller pointers, different alignment)."
echo "> sizeof(nanortc_t) is the full per-connection RAM — no heap allocation."

# Cleanup
rm -rf "$ROOT"/build-measure-*
