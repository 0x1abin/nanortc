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
| ICE RFC compliance (§8445 / §7675 / §8838) | [docs/engineering/ice-rfc-compliance.md](docs/engineering/ice-rfc-compliance.md) |
| TURN RFC compliance (§5766 / §8656) | [docs/engineering/turn-rfc-compliance.md](docs/engineering/turn-rfc-compliance.md) |
| Coding standards | [docs/engineering/coding-standards.md](docs/engineering/coding-standards.md) |
| Architecture constraints (CI checks) | [docs/engineering/architecture-constraints.md](docs/engineering/architecture-constraints.md) |
| Development workflow | [docs/engineering/development-workflow.md](docs/engineering/development-workflow.md) |
| Technical debt tracker | [docs/exec-plans/tech-debt-tracker.md](docs/exec-plans/tech-debt-tracker.md) |
| Memory profiles + tuning guide | [docs/engineering/memory-profiles.md](docs/engineering/memory-profiles.md) |
| Safe C function guidelines | [docs/engineering/safe-c-guidelines.md](docs/engineering/safe-c-guidelines.md) |

## Build

```bash
# Host build (default: DataChannel only)
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure

# Feature flags (orthogonal, any combination)
cmake -B build -DNANORTC_FEATURE_DATACHANNEL=ON   # SCTP + DCEP (default ON)
cmake -B build -DNANORTC_FEATURE_AUDIO=ON          # RTP/SRTP + jitter buffer
cmake -B build -DNANORTC_FEATURE_VIDEO=ON           # RTP/SRTP + BWE (H.264 only by default)
cmake -B build -DNANORTC_FEATURE_H265=ON            # H.265/HEVC codec (opt-in; requires VIDEO=ON)
cmake -B build -DNANORTC_FEATURE_DC_RELIABLE=OFF    # Disable retransmit (sub-feature of DC)
cmake -B build -DNANORTC_FEATURE_DC_ORDERED=OFF     # Disable ordered delivery (sub-feature of DC)
cmake -B build -DNANORTC_FEATURE_IPV6=OFF           # Disable IPv6 address support (saves ~300 bytes)
cmake -B build -DNANORTC_FEATURE_TURN=OFF           # Disable TURN relay (saves ~700B RAM + ~13KB code)
cmake -B build -DNANORTC_FEATURE_ICE_SRFLX=OFF      # Skip srflx local-candidate registration (LAN-only)

# Common combinations
cmake -B build -DNANORTC_FEATURE_DATACHANNEL=ON -DNANORTC_FEATURE_AUDIO=ON -DNANORTC_FEATURE_VIDEO=ON  # Full media (H.264 only)
cmake -B build -DNANORTC_FEATURE_DATACHANNEL=ON -DNANORTC_FEATURE_AUDIO=ON -DNANORTC_FEATURE_VIDEO=ON -DNANORTC_FEATURE_H265=ON  # Full media + H.265
cmake -B build -DNANORTC_FEATURE_DATACHANNEL=OFF -DNANORTC_FEATURE_AUDIO=ON                          # Audio only (no SCTP)

# Crypto backend: mbedtls (default, for embedded) or openssl (for Linux host)
cmake -B build -DNANORTC_CRYPTO=openssl
cmake -B build -DNANORTC_CRYPTO=mbedtls

# Build examples (Linux host, not default)
cmake -B build -DNANORTC_FEATURE_DATACHANNEL=ON -DNANORTC_FEATURE_AUDIO=ON -DNANORTC_FEATURE_VIDEO=ON \
      -DNANORTC_CRYPTO=openssl -DNANORTC_BUILD_EXAMPLES=ON

# Custom configuration (override defaults without modifying repo)
cmake -B build -DNANORTC_CONFIG_FILE=\"my_nanortc_config.h\"

# Interop tests against libdatachannel (requires OpenSSL + C++ compiler)
cmake -B build -DNANORTC_CRYPTO=openssl -DNANORTC_BUILD_INTEROP_TESTS=ON
cmake --build build -j$(nproc)
ctest --test-dir build -R interop --output-on-failure

# With AddressSanitizer
cmake -B build -DADDRESS_SANITIZER=ON

# Fuzz testing (requires LLVM clang with libFuzzer, not AppleClang)
./scripts/run-fuzz.sh            # 30s per harness (default)
./scripts/run-fuzz.sh 300        # 5min per harness
./scripts/run-fuzz.sh 30 fuzz_stun  # Single harness

# Code coverage (requires gcov + lcov)
./scripts/coverage.sh              # Generate HTML report
./scripts/coverage.sh --threshold 80  # Fail if < 80%
./scripts/coverage.sh --open       # Open report in browser

# ESP-IDF (auto-detected via IDF_PATH; use `idf.py menuconfig` for Kconfig)
idf.py build

# Format
clang-format -i src/*.c src/*.h include/*.h crypto/*.h crypto/*.c

# Run full CI locally (same checks as GitHub Actions)
./scripts/ci-check.sh             # full matrix; mirrors GitHub Actions
./scripts/ci-check.sh --fast      # tier-1 subset for tight pre-push loops (DATA + MEDIA + ASan, ~5s with ccache hit)
./scripts/ci-check.sh --clean     # wipe build dirs first
# The script auto-detects ccache and keeps build dirs across runs for
# incremental compilation. Install via: brew install ccache
```

## Mandatory Rules

These rules are mechanically enforced. Violations will break the build or CI.

**Configuration:** All compile-time tunables (buffer sizes, limits, timeouts) must be defined in `include/nanortc_config.h` with `#ifndef` guards. Use `NANORTC_*` prefix. Internal headers include `nanortc_config.h` directly. Users override via `NANORTC_CONFIG_FILE` or ESP-IDF Kconfig.

**Sans I/O discipline:** `src/` files may only include `<string.h>`, `<stdint.h>`, `<stdbool.h>`, `<stddef.h>`, and internal `nano_*.h` / `nanortc_crypto.h` / `nanortc_config.h`. No OS/platform headers.

**No dynamic allocation:** No `malloc`/`free` in `src/`. Use caller-provided buffers.

**Naming:** Public API: `nanortc_` prefix. Internal: module prefix (`stun_`, `sctp_`, etc.). Types: `nano_*_t`. Enums: `NANORTC_*`.

**Error handling:** Return `int` (0 = `NANORTC_OK`, negative = `NANORTC_ERR_*`). No `assert()` in library code.

**Byte order:** Use `nanortc_htons`/`nanortc_ntohs`/`nanortc_htonl`/`nanortc_ntohl` from `nanortc.h`. Never platform `htons`.

**Feature guards:** Code guarded by orthogonal feature flags: `#if NANORTC_FEATURE_DATACHANNEL`, `#if NANORTC_FEATURE_AUDIO`, `#if NANORTC_FEATURE_VIDEO`, `#if NANORTC_FEATURE_H265`, `#if NANORTC_FEATURE_IPV6`, `#if NANORTC_FEATURE_TURN`, `#if NANORTC_FEATURE_ICE_SRFLX`, `#if NANORTC_HAVE_MEDIA_TRANSPORT`. All 7 feature combinations must compile and pass tests (DATA, AUDIO, MEDIA, MEDIA_H265, AUDIO_ONLY, MEDIA_ONLY, CORE_ONLY). H.265 is opt-in: `NANORTC_FEATURE_VIDEO=ON` offers only H.264; set `NANORTC_FEATURE_H265=ON` to add H.265. TURN and ICE_SRFLX can be independently disabled in any combination.

**No global state:** All state in `nanortc_t`. Multiple instances must coexist.

**RFC authority:** RFC documents are the **sole authoritative source** for protocol stack implementation. Do not consult third-party implementations (e.g., str0m, libdatachannel, kvs-webrtc) when designing wire formats, state machines, or protocol behavior. Every non-obvious design decision must cite a specific RFC section in a comment.

**RFC testing:** Any RFC-based module MUST have tests generated independently from the RFC document — hardcoded byte-level test vectors from RFC appendices (e.g., RFC 5769 for STUN) plus real captures from browser/wireshark pcaps. Roundtrip tests (encode → parse own output) are supplementary only. See [development-workflow.md](docs/engineering/development-workflow.md) for full requirements.

**Safe C functions:** No `strlen`, `sprintf`, `snprintf`, `strcpy`, `strncpy`, `strcat`, `strncat`, `sscanf`, `atoi`, `atol`, `gets` in `src/` or `crypto/`. Use explicit `(buffer, length)` pairs and `memcpy`. API boundary functions (`nano_*`) may use `strlen` once per parameter with `/* NANORTC_SAFE: API boundary */` annotation. See `docs/engineering/safe-c-guidelines.md`.

**Named array sizes:** Every struct array member must use a named macro for its size, never a bare integer literal. Configurable buffer sizes use `NANORTC_*` macros in `nanortc_config.h` with `#ifndef` guards. Protocol-fixed sizes (RFC-mandated) use `MODULE_*_SIZE` macros in the relevant module header. Boundary checks in `.c` files must reference the same macro.

## Approach
- Think before acting. Read existing files before writing code.
- Be concise in output but thorough in reasoning.
- Prefer editing over rewriting whole files.
- Do not re-read files you have already read unless the file may have changed.
- Test your code before declaring done.
- No sycophantic openers or closing fluff.
- Keep solutions simple and direct.
- User instructions always override this file.
