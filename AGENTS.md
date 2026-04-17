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

## API at a Glance

One public header (`include/nanortc.h`), one state struct, two I/O functions — everything else is convenience around the loop.

```c
int  nanortc_init        (nanortc_t *rtc, const nanortc_config_t *cfg);
void nanortc_destroy     (nanortc_t *rtc);

int  nanortc_handle_input(nanortc_t *rtc, const nanortc_input_t  *in);   // in:  {now_ms, data, len, src, dst}
int  nanortc_poll_output (nanortc_t *rtc,       nanortc_output_t *out);  // out.type ∈ {TRANSMIT, EVENT, TIMEOUT}
```

Returns `0 = NANORTC_OK`, negative = `NANORTC_ERR_*`. `nanortc_input_t` with `data=NULL, len=0, src.family=0` is a pure timer tick — only `now_ms` required.

**Module graph** (dependencies flow down, no cycles): `nano_rtc` (FSM) → `nano_sdp` / `nano_ice` → `nano_stun` / `nano_dtls` → `nano_sctp` → `nano_datachannel`, plus `nano_srtp` → `nano_rtp` → `nano_rtcp` / jitter / bwe / h264 / h265 under AUDIO/VIDEO, and optional `nano_turn`. Full graph + packet lifecycle in [ARCHITECTURE.md](ARCHITECTURE.md).

## Mandatory Rules

These rules are mechanically enforced. Violations will break the build or CI.

**Sans I/O discipline:** `src/` files may only include `<string.h>`, `<stdint.h>`, `<stdbool.h>`, `<stddef.h>`, and internal `nano_*.h` / `nanortc_crypto.h` / `nanortc_config.h`. No OS/platform headers.

**Memory & state:** No `malloc`/`free` in `src/` — use caller-provided buffers. All connection state lives in `nanortc_t`; multiple instances must coexist without shared globals.

**Constants & configuration:** Compile-time tunables (buffer sizes, limits, timeouts) live in `include/nanortc_config.h` with `#ifndef` guards and `NANORTC_*` prefix. Users override via `NANORTC_CONFIG_FILE` or ESP-IDF Kconfig. Every struct array member must be sized via a named macro — configurable sizes use `NANORTC_*`, RFC-fixed sizes use `MODULE_*_SIZE` in the module header. Never a bare integer literal.

**Naming & errors:** Public API uses the `nanortc_` prefix; internal modules use `stun_`, `sctp_`, etc.; types are `nano_*_t`; enums and macros are `NANORTC_*`. Functions return `int` where `0 = NANORTC_OK` and negative values are `NANORTC_ERR_*`. No `assert()` in library code.

**Byte order:** Use `nanortc_htons` / `nanortc_ntohs` / `nanortc_htonl` / `nanortc_ntohl` from `nanortc.h`. Never the platform `htons`.

**Feature guards:** Protocol code is guarded by orthogonal flags — `DATACHANNEL`, `AUDIO`, `VIDEO`, `H265`, `IPV6`, `TURN`, `ICE_SRFLX`, plus the derived `NANORTC_HAVE_MEDIA_TRANSPORT`. All 7 canonical combinations must compile and pass tests (DATA, AUDIO, MEDIA, MEDIA_H265, AUDIO_ONLY, MEDIA_ONLY, CORE_ONLY). Verify with `./scripts/ci-check.sh`; full matrix in [docs/guide-docs/build.md](docs/guide-docs/build.md).

**RFC compliance:** RFC documents are the **sole authoritative source** for protocol stack implementation. Do not consult third-party implementations (str0m, libdatachannel, kvs-webrtc) for wire formats, state machines, or behavior — every non-obvious decision cites a specific RFC section in a comment. Tests are derived independently from the RFC: hardcoded byte-level vectors from RFC appendices (e.g. RFC 5769 for STUN) plus real browser/wireshark captures. Roundtrip tests are supplementary only. See [development-workflow.md](docs/engineering/development-workflow.md).

**Safe C functions:** No `strlen`, `sprintf`, `snprintf`, `strcpy`, `strncpy`, `strcat`, `strncat`, `sscanf`, `atoi`, `atol`, `gets` in `src/` or `crypto/`. Use explicit `(buffer, length)` pairs and `memcpy`. API boundary functions (`nano_*`) may use `strlen` once per parameter with `/* NANORTC_SAFE: API boundary */`. See [safe-c-guidelines.md](docs/engineering/safe-c-guidelines.md).

## Approach
- Think before acting. Read existing files before writing code.
- Be concise in output but thorough in reasoning.
- Prefer editing over rewriting whole files.
- Do not re-read files you have already read unless the file may have changed.
- Test your code before declaring done.
- No sycophantic openers or closing fluff.
- Keep solutions simple and direct.
- User instructions always override this file.
