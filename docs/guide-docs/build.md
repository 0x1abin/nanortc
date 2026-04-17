# Build Guide

Full reference for building, testing, and verifying NanoRTC locally. For the minimum cold-start command set, see [AGENTS.md § Build](../../AGENTS.md#build).

## Prerequisites

- CMake ≥ 3.13, a C99 compiler (GCC, Clang, or AppleClang)
- `clang-format` — required for source formatting checks
- `ccache` (optional but recommended) — `brew install ccache`; auto-detected by `scripts/ci-check.sh`
- OpenSSL — required for `NANORTC_CRYPTO=openssl` and interop tests
- LLVM `clang` with libFuzzer — required for fuzz harnesses (AppleClang does not ship libFuzzer)
- `gcov` + `lcov` — required for coverage reports
- ESP-IDF toolchain — required for the ESP32 target (auto-detected via `IDF_PATH`)

## Host Build

Default build: DataChannel only, debug symbols, run unit tests.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

## Feature Flags

Flags are orthogonal — any combination is supported. The build system enforces that all 7 canonical combinations compile and pass tests (DATA, AUDIO, MEDIA, MEDIA_H265, AUDIO_ONLY, MEDIA_ONLY, CORE_ONLY).

```bash
cmake -B build -DNANORTC_FEATURE_DATACHANNEL=ON   # SCTP + DCEP (default ON)
cmake -B build -DNANORTC_FEATURE_AUDIO=ON          # RTP/SRTP + jitter buffer
cmake -B build -DNANORTC_FEATURE_VIDEO=ON           # RTP/SRTP + BWE (H.264 only by default)
cmake -B build -DNANORTC_FEATURE_H265=ON            # H.265/HEVC codec (opt-in; requires VIDEO=ON)
cmake -B build -DNANORTC_FEATURE_DC_RELIABLE=OFF    # Disable retransmit (sub-feature of DC)
cmake -B build -DNANORTC_FEATURE_DC_ORDERED=OFF     # Disable ordered delivery (sub-feature of DC)
cmake -B build -DNANORTC_FEATURE_IPV6=OFF           # Disable IPv6 address support (saves ~300 bytes)
cmake -B build -DNANORTC_FEATURE_TURN=OFF           # Disable TURN relay (saves ~700B RAM + ~13KB code)
cmake -B build -DNANORTC_FEATURE_ICE_SRFLX=OFF      # Skip srflx local-candidate registration (LAN-only)
```

Common combinations:

```bash
# Full media (H.264 only)
cmake -B build -DNANORTC_FEATURE_DATACHANNEL=ON -DNANORTC_FEATURE_AUDIO=ON -DNANORTC_FEATURE_VIDEO=ON

# Full media + H.265
cmake -B build -DNANORTC_FEATURE_DATACHANNEL=ON -DNANORTC_FEATURE_AUDIO=ON -DNANORTC_FEATURE_VIDEO=ON -DNANORTC_FEATURE_H265=ON

# Audio only (no SCTP)
cmake -B build -DNANORTC_FEATURE_DATACHANNEL=OFF -DNANORTC_FEATURE_AUDIO=ON
```

For RAM/flash footprint per combination and tuning knobs, see [memory-profiles.md](../engineering/memory-profiles.md).

## Crypto Backend

Pick one. `mbedtls` is the default and targets embedded; `openssl` is typical for Linux host development and required for interop tests.

```bash
cmake -B build -DNANORTC_CRYPTO=openssl
cmake -B build -DNANORTC_CRYPTO=mbedtls
```

## Examples & Custom Config

Examples are Linux-host applications and are not built by default.

```bash
# Build examples (Linux host, full media)
cmake -B build -DNANORTC_FEATURE_DATACHANNEL=ON -DNANORTC_FEATURE_AUDIO=ON -DNANORTC_FEATURE_VIDEO=ON \
      -DNANORTC_CRYPTO=openssl -DNANORTC_BUILD_EXAMPLES=ON
```

Override compile-time tunables without modifying the repo:

```bash
cmake -B build -DNANORTC_CONFIG_FILE=\"my_nanortc_config.h\"
```

## Interop Tests

Runs end-to-end tests against libdatachannel. Requires OpenSSL and a C++ compiler.

```bash
cmake -B build -DNANORTC_CRYPTO=openssl -DNANORTC_BUILD_INTEROP_TESTS=ON
cmake --build build -j$(nproc)
ctest --test-dir build -R interop --output-on-failure
```

## AddressSanitizer

```bash
cmake -B build -DADDRESS_SANITIZER=ON
```

## Fuzz Testing

Requires LLVM `clang` with libFuzzer (not AppleClang).

```bash
./scripts/run-fuzz.sh            # 30s per harness (default)
./scripts/run-fuzz.sh 300        # 5min per harness
./scripts/run-fuzz.sh 30 fuzz_stun  # Single harness
```

## Code Coverage

Requires `gcov` and `lcov`.

```bash
./scripts/coverage.sh              # Generate HTML report
./scripts/coverage.sh --threshold 80  # Fail if < 80%
./scripts/coverage.sh --open       # Open report in browser
```

## ESP-IDF

Auto-detected via `IDF_PATH`. Use `idf.py menuconfig` to adjust Kconfig knobs (feature flags, buffer sizes).

```bash
idf.py build
```

## Formatting

```bash
clang-format -i src/*.c src/*.h include/*.h crypto/*.h crypto/*.c
```

## CI Locally

`scripts/ci-check.sh` runs the same matrix as GitHub Actions. It auto-detects `ccache` and keeps build directories across runs for incremental compilation.

```bash
./scripts/ci-check.sh             # full matrix; mirrors GitHub Actions
./scripts/ci-check.sh --fast      # tier-1 subset for tight pre-push loops (DATA + MEDIA + ASan, ~5s with ccache hit)
./scripts/ci-check.sh --clean     # wipe build dirs first
```
