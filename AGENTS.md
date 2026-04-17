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
| Build guide (flags, crypto, fuzz, coverage, ESP-IDF) | [docs/guide-docs/build.md](docs/guide-docs/build.md) |
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

Cold start — debug build, run tests, run CI check:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
./scripts/ci-check.sh --fast   # tier-1 pre-push (~5s with ccache)
```

Feature flags, crypto backends, examples, interop, ASan, fuzz, coverage, ESP-IDF, formatting: see [docs/guide-docs/build.md](docs/guide-docs/build.md).

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
