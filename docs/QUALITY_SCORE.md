# Quality Score

Per-module quality grades for NanoRTC. Updated as implementation progresses.

## Grading Scale

| Grade | Meaning |
|-------|---------|
| **A** | Complete, tested with real browser, fuzz-tested, reviewed |
| **B** | Functional, unit-tested with synthetic data, RFC-compliant |
| **B-** | Implemented and tested, but missing some edge cases or interop testing |
| **C** | Partially implemented, basic tests pass |
| **D** | Stub only — compiles but returns NOT_IMPLEMENTED |
| **—** | Not applicable for current profile |

## Module Grades

### Core (all profiles)

| Module | File | Grade | Tests | Notes |
|--------|------|-------|-------|-------|
| Main FSM | `nano_rtc.c` | **B-** | init/destroy, poll, demux, timeout, ICE→DTLS→SCTP→DC pipeline | RFC 7983 demux, full pipeline integration, SCTP timeout pump, remote candidate API. Stubs: `create_offer`, `accept_answer`. |
| STUN codec | `nano_stun.c` | **B** | 40 tests (RFC 5769 vectors, str0m, roundtrip, edge cases) | Full parser/encoder, MI (HMAC-SHA1), FP (CRC-32), ERROR-CODE. Safe byte access. |
| ICE | `nano_ice.c` | **B** | 17 tests (§7.1.1, §7.2.1, §7.3, §8, credentials) | Dual-role FSM, controlled + controlling, pacing, nomination |
| DTLS | `nano_dtls.c` | **B** | 9 tests (handshake loopback, encrypt/decrypt, keying material, fingerprint) | Sans I/O BIO adapter, ECDSA P-256 self-signed cert, RFC 5764 key export |
| SCTP-Lite | `nano_sctp.c` | **B-** | 27 tests (codec, CRC, handshake, data exchange, SACK, FORWARD-TSN, output queue) | Full codec + 4-way handshake FSM + send queue + SACK + retransmit + heartbeat + ring output queue. Missing: gap tracking, RECONFIG, SHUTDOWN-ACK. |
| DataChannel | `nano_datachannel.c` | **B-** | DCEP codec + FSM tested via SCTP e2e | DCEP OPEN/ACK codec, channel management, bidirectional FSM. Missing: partial reliability, RECONFIG. |
| SDP | `nano_sdp.c` | **B-** | 11 tests (Chrome/Firefox/Safari offers, generator, roundtrip, accept_offer) | Parser + generator. Chrome/Firefox/Safari SDP compat tested. |
| CRC-32c | `nano_crc32c.c` | **B** | test vector verified | 100% — Castagnoli polynomial for SCTP checksums |
| CRC-32 | `nano_crc32.c` | **B** | test vector verified | 100% — ISO HDLC polynomial for STUN FINGERPRINT |

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
| Crypto provider interface | **B** | Interface complete; HMAC-SHA1 + CSPRNG + DTLS (both backends); SRTP stubs remain |
| Build system (CMake) | **B** | 3 profiles, 2 crypto backends, ESP-IDF detection, `-fvisibility=hidden` |
| Test infrastructure | **B** | Shared macros (`nano_test.h`), 90+ tests across 7 suites, RFC 5769 vectors, e2e ICE+DTLS loopback |
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
1. ~~STUN parser needs real implementation (D → B)~~ **DONE** — RFC 8489 codec with RFC 5769 test vectors
2. ~~Crypto hmac_sha1 + random_bytes~~ **DONE** — both mbedtls and OpenSSL backends
3. ~~DTLS handshake needs real implementation (D → B)~~ **DONE** — Sans I/O BIO adapter + both crypto backends
4. ~~SCTP, DataChannel, SDP all still D~~ **DONE** — All at B- with full codec, FSM, tests
5. Browser end-to-end validation — Phase 1 remaining milestone

**Acceptable gaps (address in later phases):**
1. No fuzz testing yet (Phase 4)
2. No ESP32 hardware validation yet (Phase 1 Week 4)
3. No code coverage measurement (Phase 4)
4. SCTP gap tracking / RECONFIG not yet implemented (needed for robust browser interop)
