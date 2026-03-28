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
| Main FSM | `nano_rtc.c` | **B** | 19 e2e tests: init, demux, ICE→DTLS→SCTP→DC pipeline, offer/answer roundtrip, DC create/close/label, graceful close, state transitions, ICE multi-candidate | RFC 7983 demux, full pipeline integration, all public API implemented. Refactored: 5 static helpers eliminate duplication. ICE_STATE_CHANGE events emitted. Audio path: SRTP key derivation on DTLS connected, random initial SSRC/seq (RFC 3550). |
| STUN codec | `nano_stun.c` | **B** | 40 tests (RFC 5769 vectors, str0m, roundtrip, edge cases) | Full parser/encoder, MI (HMAC-SHA1), FP (CRC-32), ERROR-CODE. Safe byte access. |
| ICE | `nano_ice.c` | **B** | 17 tests (§7.1.1, §7.2.1, §7.3, §8, credentials) | Dual-role FSM, controlled + controlling, pacing, nomination |
| DTLS | `nano_dtls.c` | **B** | 9 tests (handshake loopback, encrypt/decrypt, keying material, fingerprint) | Sans I/O BIO adapter, ECDSA P-256 self-signed cert, RFC 5764 key export |
| SCTP-Lite | `nano_sctp.c` | **B-** | 27 tests (codec, CRC, handshake, data exchange, SACK, FORWARD-TSN, output queue) | Full codec + 4-way handshake FSM + send queue + SACK + retransmit + heartbeat + ring output queue. Missing: gap tracking, RECONFIG, SHUTDOWN-ACK. |
| DataChannel | `nano_datachannel.c` | **B-** | DCEP codec + FSM tested via SCTP e2e | DCEP OPEN/ACK codec, channel management, bidirectional FSM. Missing: partial reliability, RECONFIG. |
| SDP | `nano_sdp.c` | **B** | 11 tests (Chrome/Firefox/Safari offers, generator, roundtrip, accept_offer) | Parser + generator refactored with helper functions. Chrome/Firefox/Safari SDP compat. Audio m-line with Opus/ptime:20. M-line ordering matches offer (RFC 8829). |
| CRC-32c | `nano_crc32c.c` | **B** | test vector verified | 100% — Castagnoli polynomial for SCTP checksums |
| CRC-32 | `nano_crc32.c` | **B** | test vector verified | 100% — ISO HDLC polynomial for STUN FINGERPRINT |

### Audio (AUDIO/MEDIA profiles)

| Module | File | Grade | Tests | Notes |
|--------|------|-------|-------|-------|
| RTP | `nano_rtp.c` | **B** | 8 tests (byte vectors, roundtrip, marker bit, error cases) | RFC 3550 pack/unpack, V=2/CSRC/extension header support |
| RTCP | `nano_rtcp.c` | **B** | 15 tests (SR/RR/NACK generate + parse, roundtrip, error cases) | RFC 3550 SR/RR, RFC 4585 Generic NACK, parser with validation |
| SRTP | `nano_srtp.c` | **B** | 6 tests (RFC 3711 B.3 key derivation vectors, protect/unprotect roundtrip, tamper detection, multi-packet) | RFC 3711 AES-128-CM-HMAC-SHA1-80, key derivation, protect/unprotect with ROC tracking. IV computation verified against libsrtp/str0m (packet_index bytes 6-13). |
| Jitter | `nano_jitter.c` | **B-** | 8 tests (push/pop, reorder, wraparound, playout delay, overflow) | Fixed ring buffer with playout delay and reordering |

### Video (MEDIA profile)

| Module | File | Grade | Notes |
|--------|------|-------|-------|
| BWE | `nano_bwe.c` | **D** | Bandwidth estimation stub |

### Infrastructure

| Component | Grade | Notes |
|-----------|-------|-------|
| Crypto provider interface | **B** | Interface complete; HMAC-SHA1 + CSPRNG + DTLS + AES-128-CM + HMAC-SHA1-80 (both backends) |
| Build system (CMake) | **B** | 3 profiles, 2 crypto backends, ESP-IDF detection, `-fvisibility=hidden` |
| Test infrastructure | **B** | Shared macros (`nano_test.h`), 140+ tests across 11 suites, RFC 5769/3711 vectors, e2e ICE+DTLS loopback, full public API coverage |
| Interop test framework | **B** | libdatachannel v0.22.5 as reference peer, 5 interop tests all pass (handshake, DC open, text/binary). SDP compat fixed (commit `4d143f2`). |
| CI pipeline | **B** | GitHub Actions: 3-profile × 2-crypto matrix, constraints, ASan. Local: `scripts/ci-check.sh` |
| Examples | **B-** | Linux datachannel + media_send templates + browser interop (HTTP signaling + `signaling_server.py`). Browser audio verified: Opus → Chrome with 0% concealed samples. Includes opus_verify + opus_gen_tone tools. ESP32 example planned. |
| Documentation | **B** | AGENTS.md, ARCHITECTURE.md, exec plans, quality scores, core beliefs, RFC index |

## Quality Targets

| Phase | Target |
|-------|--------|
| Phase 1 complete | All core modules at **B** or above + all interop tests pass ✓ + browser & ESP32 integration verified |
| Phase 2 complete | All audio modules at **B** or above |
| Phase 3 complete | All modules at **B** or above |
| Phase 4 | All modules at **A** (fuzz-tested, browser-verified, interop-verified) |

## Gap Analysis

**Critical gaps (must fix before Phase 1 milestones):**
1. ~~STUN parser needs real implementation (D → B)~~ **DONE** — RFC 8489 codec with RFC 5769 test vectors
2. ~~Crypto hmac_sha1 + random_bytes~~ **DONE** — both mbedtls and OpenSSL backends
3. ~~DTLS handshake needs real implementation (D → B)~~ **DONE** — Sans I/O BIO adapter + both crypto backends
4. ~~SCTP, DataChannel, SDP all still D~~ **DONE** — All at B- with full codec, FSM, tests
5. ~~No interop testing framework~~ **DONE** — libdatachannel interop framework with 5 test cases
6. ~~Interop tests must all pass~~ **DONE** — 5/5 pass after SDP compat fix (commit `4d143f2`)
7. ~~Browser end-to-end validation~~ **DONE** — DataChannel + Audio (Opus → Chrome) verified via browser_interop example
8. ESP32 hardware validation — Phase 1 remaining milestone (HTTP signaling, reusing `http_signaling.c`)

**Acceptable gaps (address in later phases):**
1. No fuzz testing yet (Phase 4)
2. No ESP32 hardware validation yet (Phase 1 — HTTP signaling via `http_signaling.c`)
3. No code coverage measurement (Phase 4)
4. SCTP gap tracking / RECONFIG not yet implemented (needed for robust browser interop)
