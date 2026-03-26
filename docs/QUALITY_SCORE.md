# Quality Score

Per-module quality grades for NanoRTC. Updated as implementation progresses.

## Grading Scale

| Grade | Meaning |
|-------|---------|
| **A** | Complete, tested with real browser, fuzz-tested, reviewed |
| **B** | Functional, unit-tested with synthetic data, RFC-compliant |
| **C** | Partially implemented, basic tests pass |
| **D** | Stub only — compiles but returns NOT_IMPLEMENTED |
| **—** | Not applicable for current profile |

## Module Grades

### Core (all profiles)

| Module | File | Grade | Tests | Coverage | Notes |
|--------|------|-------|-------|----------|-------|
| Main FSM | `nano_rtc.c` | **D** | init/destroy, poll_empty | — | Stub dispatcher |
| STUN codec | `nano_stun.c` | **D** | header_size, too_short | — | Parser/encoder stubs |
| ICE | `nano_ice.c` | **D** | — | — | Controlled + controlling roles, stub |
| DTLS | `nano_dtls.c` | **D** | — | — | BIO adapter stub |
| SCTP-Lite | `nano_sctp.c` | **D** | — | — | Most complex module |
| DataChannel | `nano_datachannel.c` | **D** | — | — | DCEP stub |
| SDP | `nano_sdp.c` | **D** | — | — | Parser/generator stub |
| CRC-32c | `nano_crc32c.c` | **B** | test vector verified | 100% | Complete implementation |

### Audio (AUDIO/MEDIA profiles)

| Module | File | Grade | Tests | Notes |
|--------|------|-------|-------|-------|
| RTP | `nano_rtp.c` | **D** | — | Codec packetization stub |
| RTCP | `nano_rtcp.c` | **D** | — | SR/RR/NACK stub |
| SRTP | `nano_srtp.c` | **D** | — | Crypto integration stub |
| Jitter | `nano_jitter.c` | **D** | — | Ring buffer stub |

### Video (MEDIA profile)

| Module | File | Grade | Notes |
|--------|------|-------|-------|
| BWE | `nano_bwe.c` | **D** | Bandwidth estimation stub |

### Infrastructure

| Component | Grade | Notes |
|-----------|-------|-------|
| Crypto provider interface | **B** | Interface complete, dual backend (mbedtls + OpenSSL stubs) |
| Build system (CMake) | **B** | 3 profiles, 2 crypto backends, ESP-IDF detection, `-fvisibility=hidden` |
| Test infrastructure | **B** | Shared macros (`nano_test.h`), 12 tests across 3 suites, e2e framework |
| CI pipeline | **B** | GitHub Actions: 3-profile × 2-crypto matrix, constraints, ASan. Local: `scripts/ci-check.sh` |
| Examples | **C** | Linux datachannel + media_send templates, media sample submodule. Not yet tested with real connections. |
| Documentation | **B** | AGENTS.md, ARCHITECTURE.md, exec plans, quality scores, core beliefs, RFC index |

## Quality Targets

| Phase | Target |
|-------|--------|
| Phase 1 complete | All core modules at **B** or above |
| Phase 2 complete | All audio modules at **B** or above |
| Phase 3 complete | All modules at **B** or above |
| Phase 4 | All modules at **A** (fuzz-tested, browser-verified) |

## Gap Analysis

**Critical gaps (must fix before Phase 1 milestones):**
1. STUN parser needs real implementation (D → B)
2. Crypto provider needs real mbedtls + OpenSSL implementations (D → B)

**Acceptable gaps (address in later phases):**
1. No fuzz testing yet (Phase 4)
2. No ESP32 hardware validation yet (Phase 1 Week 4)
3. No code coverage measurement (Phase 4)
