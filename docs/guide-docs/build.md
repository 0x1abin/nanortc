# Build Guide

Full reference for building, testing, and verifying NanoRTC locally. For the minimum cold-start command set, see [AGENTS.md § Build](../../AGENTS.md#build).

## Prerequisites

- CMake ≥ 3.16, a C99 compiler (GCC, Clang, or AppleClang)
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

Auto-detected via `IDF_PATH`. The standard way to load the ESP-IDF environment once per shell session (exports `IDF_PATH`, `idf.py`, and the cross-toolchain) is the `get_idf` alias from the ESP-IDF install script:

```bash
get_idf
```

`get_idf` is defined by ESP-IDF's `install.sh` / `install.fish` and typically resolves to something like `alias get_idf='. $HOME/esp/esp-idf/export.sh'`. If it's not defined in your shell, source `export.sh` directly from wherever your IDF checkout lives.

Configure, build, flash, and view logs from the project directory (for example `examples/esp32_datachannel/`):

```bash
idf.py set-target esp32p4      # one-time per project; esp32s3 / esp32c6 also supported
idf.py menuconfig               # optional — adjust Kconfig knobs (feature flags, buffer sizes)
idf.py build                    # compile firmware
idf.py -p /dev/tty.usbmodem* flash   # write firmware to the attached board
idf.py -p /dev/tty.usbmodem* monitor # serial log viewer (Ctrl-] to quit)
idf.py -p /dev/tty.usbmodem* flash monitor   # flash then immediately open monitor
```

Omit `-p` to let `idf.py` auto-detect the USB serial port. On Linux the device usually appears as `/dev/ttyUSB0` or `/dev/ttyACM0`.

### Board-manager prebuild (esp32_camera only)

The `esp32_camera` example uses [`esp_board_manager`](https://components.espressif.com/components/espressif/esp_board_manager) for sensor / codec / LDO wiring. Before the first `idf.py build` (and after `idf.py fullclean` or a board switch), generate `components/gen_bmgr_codes/board_manager.defaults` from the YAML under `boards/`:

```bash
cd examples/esp32_camera
idf.py set-target esp32p4
python managed_components/espressif__esp_board_manager/gen_bmgr_config_codes.py \
       -b esp32_p4_nano -c boards
idf.py build
```

The generated file is board-specific and intentionally not tracked in git. `CMakeLists.txt` now fails loud at configure time if it's missing rather than tripping on a far less helpful `dev_audio_codec.h: No such file or directory` during compile. Other `examples/esp32_*` targets don't need this step.

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
