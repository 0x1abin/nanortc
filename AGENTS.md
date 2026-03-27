# AGENTS.md

This file provides guidance to AI coding agents when working with code in this repository.

## Project

NanoRTC is a Sans I/O, pure C WebRTC implementation for RTOS/embedded systems.

## Where to Look

| What you need | Where to find it |
|---------------|-----------------|
| Architecture overview + module dependency graph | [ARCHITECTURE.md](ARCHITECTURE.md) |
| Full design specification (authoritative) | [docs/design-docs/nanortc-design-draft.md](docs/design-docs/nanortc-design-draft.md) |
| Design principles and core beliefs | [docs/design-docs/core-beliefs.md](docs/design-docs/core-beliefs.md) |
| Current execution plan | [docs/PLANS.md](docs/PLANS.md) |
| Module quality grades | [docs/QUALITY_SCORE.md](docs/QUALITY_SCORE.md) |
| RFC reference index | [docs/references/rfc-index.md](docs/references/rfc-index.md) |
| Coding standards | [docs/engineering/coding-standards.md](docs/engineering/coding-standards.md) |
| Architecture constraints (CI checks) | [docs/engineering/architecture-constraints.md](docs/engineering/architecture-constraints.md) |
| Development workflow | [docs/engineering/development-workflow.md](docs/engineering/development-workflow.md) |
| Technical debt tracker | [docs/exec-plans/tech-debt-tracker.md](docs/exec-plans/tech-debt-tracker.md) |

## Build

```bash
# Host build (default: DataChannel only)
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure

# Feature flags (orthogonal, any combination)
cmake -B build -DNANO_FEATURE_DATACHANNEL=ON   # SCTP + DCEP (default ON)
cmake -B build -DNANO_FEATURE_AUDIO=ON          # RTP/SRTP + jitter buffer
cmake -B build -DNANO_FEATURE_VIDEO=ON           # RTP/SRTP + BWE
cmake -B build -DNANO_FEATURE_DC_RELIABLE=OFF    # Disable retransmit (sub-feature of DC)
cmake -B build -DNANO_FEATURE_DC_ORDERED=OFF     # Disable ordered delivery (sub-feature of DC)

# Common combinations
cmake -B build -DNANO_FEATURE_DATACHANNEL=ON -DNANO_FEATURE_AUDIO=ON -DNANO_FEATURE_VIDEO=ON  # Full media
cmake -B build -DNANO_FEATURE_DATACHANNEL=OFF -DNANO_FEATURE_AUDIO=ON                          # Audio only (no SCTP)

# Crypto backend: mbedtls (default, for embedded) or openssl (for Linux host)
cmake -B build -DNANORTC_CRYPTO=openssl
cmake -B build -DNANORTC_CRYPTO=mbedtls

# Build examples (Linux host, not default)
cmake -B build -DNANO_FEATURE_DATACHANNEL=ON -DNANO_FEATURE_AUDIO=ON -DNANO_FEATURE_VIDEO=ON \
      -DNANORTC_CRYPTO=openssl -DNANORTC_BUILD_EXAMPLES=ON

# Custom configuration (override defaults without modifying repo)
cmake -B build -DNANORTC_CONFIG_FILE=\"my_nanortc_config.h\"

# Interop tests against libdatachannel (requires OpenSSL + C++ compiler)
cmake -B build -DNANORTC_CRYPTO=openssl -DNANORTC_BUILD_INTEROP_TESTS=ON
cmake --build build -j$(nproc)
ctest --test-dir build -R interop --output-on-failure

# With AddressSanitizer
cmake -B build -DADDRESS_SANITIZER=ON

# ESP-IDF (auto-detected via IDF_PATH; use `idf.py menuconfig` for Kconfig)
idf.py build

# Format
clang-format -i src/*.c src/*.h include/*.h crypto/*.h crypto/*.c

# Run full CI locally (same checks as GitHub Actions)
./scripts/ci-check.sh
```

## Mandatory Rules

These rules are mechanically enforced. Violations will break the build or CI.

**Configuration:** All compile-time tunables (buffer sizes, limits, timeouts) must be defined in `include/nanortc_config.h` with `#ifndef` guards. Use `NANO_*` prefix. Internal headers include `nanortc_config.h` directly. Users override via `NANORTC_CONFIG_FILE` or ESP-IDF Kconfig.

**Sans I/O discipline:** `src/` files may only include `<string.h>`, `<stdint.h>`, `<stdbool.h>`, `<stddef.h>`, and internal `nano_*.h` / `nano_crypto.h` / `nanortc_config.h`. No OS/platform headers.

**No dynamic allocation:** No `malloc`/`free` in `src/`. Use caller-provided buffers.

**Naming:** Public API: `nano_` prefix. Internal: module prefix (`stun_`, `sctp_`, etc.). Types: `nano_*_t`. Enums: `NANO_*`.

**Error handling:** Return `int` (0 = `NANO_OK`, negative = `NANO_ERR_*`). No `assert()` in library code.

**Byte order:** Use `nano_htons`/`nano_ntohs`/`nano_htonl`/`nano_ntohl` from `nanortc.h`. Never platform `htons`.

**Feature guards:** Code guarded by orthogonal feature flags: `#if NANO_FEATURE_DATACHANNEL`, `#if NANO_FEATURE_AUDIO`, `#if NANO_FEATURE_VIDEO`, `#if NANO_HAVE_MEDIA_TRANSPORT`. All 6 feature combinations must compile and pass tests (DATA, AUDIO, MEDIA, AUDIO_ONLY, MEDIA_ONLY, CORE_ONLY).

**No global state:** All state in `nano_rtc_t`. Multiple instances must coexist.

**RFC authority:** When RFC and reference code disagree, RFC wins. Cite RFC sections in comments.

**RFC testing:** Any RFC-based module MUST have tests generated independently from the RFC document — hardcoded byte-level test vectors from RFC appendices (e.g., RFC 5769 for STUN) plus real captures from reference implementations. Roundtrip tests (encode → parse own output) are supplementary only. See [development-workflow.md](docs/engineering/development-workflow.md) for full requirements.

**Safe C functions:** No `strlen`, `sprintf`, `snprintf`, `strcpy`, `strncpy`, `strcat`, `strncat`, `sscanf`, `atoi`, `atol`, `gets` in `src/` or `crypto/`. Use explicit `(buffer, length)` pairs and `memcpy`. API boundary functions (`nano_*`) may use `strlen` once per parameter with `/* NANO_SAFE: API boundary */` annotation. See `docs/engineering/safe-c-guidelines.md`.

**Named array sizes:** Every struct array member must use a named macro for its size, never a bare integer literal. Configurable buffer sizes use `NANO_*` macros in `nanortc_config.h` with `#ifndef` guards. Protocol-fixed sizes (RFC-mandated) use `MODULE_*_SIZE` macros in the relevant module header. Boundary checks in `.c` files must reference the same macro.

## Reference Implementations

- `.local-reference/str0m/` — Rust Sans I/O WebRTC (architecture reference for poll/handle pattern)
- `.local-reference/libdatachannel/` — C/C++ WebRTC network library featuring Data Channels, Media Transport, and WebSockets
- `.local-reference/amazon-kinesis-video-streams-webrtc-sdk-c/` — beta-reference-esp-port

Consult str0m for Sans I/O patterns.
