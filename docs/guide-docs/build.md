# Build Guide

Full reference for building, testing, and verifying NanoRTC locally. For the minimum cold-start command set, see [AGENTS.md § Build](../../AGENTS.md#build).

## Prerequisites

- CMake ≥ 3.16, a C99 compiler (GCC, Clang, or AppleClang)
- `clang-format` — required for source formatting checks
- `ccache` (optional but recommended) — `brew install ccache`; auto-detected by `scripts/ci-check.sh`
- OpenSSL — required for `NANORTC_CRYPTO=openssl` and interop tests
- LLVM `clang` with libFuzzer — required for fuzz harnesses (AppleClang does not ship libFuzzer)
- `gcov` + `lcov` — required for coverage reports
- ESP-IDF toolchain — required for ESP32 targets. Component mode is selected by
  ESP-IDF's `ESP_PLATFORM`; merely exporting `IDF_PATH` no longer changes a host build.

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
cmake -B build -DNANORTC_FEATURE_VIDEO_RATE_CONTROL=ON # Adaptive encoder-spec recommendations
cmake -B build -DNANORTC_FEATURE_VIDEO_REORDER=ON   # Receive-side RTP reorder window
cmake -B build -DNANORTC_FEATURE_VIDEO_NACK_RX=ON   # Receiver-generated RTCP NACK
cmake -B build -DNANORTC_FEATURE_VIDEO_FEC=ON       # ULPFEC (requires REORDER=ON)
cmake -B build -DNANORTC_FEATURE_VIDEO_PACING=OFF   # Disable the default send pacer
cmake -B build -DNANORTC_FEATURE_VIDEO_AUTO_PLI=OFF # Application owns keyframe recovery
cmake -B build -DNANORTC_FEC_ADAPTIVE=OFF           # Fixed rather than loss-adaptive FEC group
```

Common combinations:

```bash
# Full media (H.264 only)
cmake -B build -DNANORTC_FEATURE_DATACHANNEL=ON -DNANORTC_FEATURE_AUDIO=ON -DNANORTC_FEATURE_VIDEO=ON

# Full media + H.265
cmake -B build -DNANORTC_FEATURE_DATACHANNEL=ON -DNANORTC_FEATURE_AUDIO=ON -DNANORTC_FEATURE_VIDEO=ON -DNANORTC_FEATURE_H265=ON

# Audio only (no SCTP)
cmake -B build -DNANORTC_FEATURE_DATACHANNEL=OFF -DNANORTC_FEATURE_AUDIO=ON

# Advanced loss recovery + adaptive control
cmake -B build \
  -DNANORTC_FEATURE_AUDIO=ON -DNANORTC_FEATURE_VIDEO=ON \
  -DNANORTC_FEATURE_VIDEO_RATE_CONTROL=ON \
  -DNANORTC_FEATURE_VIDEO_REORDER=ON \
  -DNANORTC_FEATURE_VIDEO_NACK_RX=ON \
  -DNANORTC_FEATURE_VIDEO_FEC=ON
```

For RAM/flash footprint per combination and tuning knobs, see [memory-profiles.md](../engineering/memory-profiles.md).

## Crypto Backend

Pick one. `mbedtls` is the default and targets embedded; `openssl` is typical for Linux host development and required for interop tests.
Any other value is rejected during CMake configure instead of silently falling
back to mbedTLS.

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

The transient control/audio backing ring can also be selected directly from
CMake with `-DNANORTC_TX_SLOT_COUNT=4`. Valid values are powers of two from 1
through 32; the value must not exceed `NANORTC_OUT_QUEUE_SIZE`.

## Interop Tests

Runs end-to-end tests against libdatachannel. Requires OpenSSL and a C++ compiler.

```bash
cmake -B build -DNANORTC_CRYPTO=openssl -DNANORTC_BUILD_INTEROP_TESTS=ON \
      -DNANORTC_FEATURE_AUDIO=ON -DNANORTC_FEATURE_VIDEO=ON -DNANORTC_FEATURE_H265=ON
cmake --build build -j$(nproc)
ctest --test-dir build -N | grep -E 'interop_(audio|video)'
ctest --test-dir build -R interop -LE network --output-on-failure
```

The real-network TURN suite is opt-in. Without a reachable external relay it
returns CTest skip code 77; strict mode converts missing credentials,
unreachable service, or an unusable single-host topology into a failure:

```bash
export NANORTC_INTEROP_NETWORK_REQUIRED=1
export NANORTC_TURN_URL=turn:turn.example.com:3478
export NANORTC_STUN_URL=stun:turn.example.com:3478
export NANORTC_TURN_USER="$TURN_USER"
export NANORTC_TURN_PASS="$TURN_PASS"
ctest --test-dir build -R '^interop_turn$' --output-on-failure
ctest --test-dir build -L turn-relay --output-on-failure
```

GitHub Actions runs this only by manual dispatch from
`.github/workflows/interop-turn-external.yml`. Configure a protected
`turn-interop` environment with a required reviewer, `COTURN_AUTH_SECRET`, and
the `NANORTC_TURN_URL` variable. The single-concurrency workflow builds before
accessing the secret, derives a 30-minute TURN REST credential, and does not
expose the shared secret to pull-request jobs. It may use an existing compatible
coturn without changing or restarting the server, but each run creates real
relay allocations and traffic.
Deployment and secret-rotation guidance is in
[turn-interop-ci.md](turn-interop-ci.md).

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

The fuzz build instruments both each harness and `libnanortc.a`, discovers
`tests/fuzz/fuzz_*.c` targets automatically, and includes a dedicated TWCC
feedback parser harness. Complete sanitizer output is retained under
`build-fuzz/fuzz-logs/` and replayed when a harness fails.

## Code Coverage

Requires `gcov` and `lcov`.

```bash
./scripts/coverage.sh              # Generate HTML report
./scripts/coverage.sh --threshold 80  # Fail if < 80%
./scripts/coverage.sh --open       # Open report in browser
```

## ESP-IDF

ESP-IDF sets `ESP_PLATFORM` while evaluating nanortc as a component; that is
the build-mode boundary. The standard way to load the toolchain for `idf.py`
(exports `IDF_PATH`, `idf.py`, and the cross-toolchain) is the `get_idf` alias:

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

The `esp32_camera` example uses [`esp_board_manager`](https://components.espressif.com/components/espressif/esp_board_manager) for sensor / codec / LDO wiring. The per-board `components/gen_bmgr_codes/board_manager.defaults` file is generated from the YAML under `boards/` and intentionally not tracked in git.

When `boards/` contains exactly one board directory (the default — `esp32_p4_nano`), `CMakeLists.txt` auto-runs the generator at configure time, so `idf.py set-target esp32p4 && idf.py build` just works.

Run the generator manually only when you have multiple boards under `boards/` (e.g. during custom-board bring-up) and need to pick:

```bash
python managed_components/espressif__esp_board_manager/gen_bmgr_config_codes.py \
       -b <board-name> -c boards
```

If the auto-generator ever fails, configure aborts with the generator's stderr; inspect `components/gen_bmgr_codes/` for partial output. Other `examples/esp32_*` targets don't use `esp_board_manager` and don't need this step.

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

In addition to the seven canonical profiles, CI compiles the advanced video
intersection, FEC with receiver NACK and automatic PLI disabled, and fixed-rate
FEC with video pacing disabled. Each check writes a log under
`.cache/ci/logs/`; the final 100 lines are replayed on failure instead of being
discarded.
