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
| Main FSM | `nano_rtc.c` | **B** | 25 e2e tests: init, demux, ICE→DTLS→SCTP→DC pipeline, offer/answer roundtrip, DC create/close/label, graceful close, state transitions, ICE multi-candidate, IPv6 candidates | RFC 7983 demux, full pipeline integration, all public API implemented. Refactored: `direction_complement()`, `rtc_apply_negotiated_media()` helpers. IPv6 candidate parsing via `nano_addr`. Video send path: H.264 FU-A → RTP → SRTP → per-slot pkt_ring. Audio+Video PT applied in both accept_offer and accept_answer. |
| STUN codec | `nano_stun.c` | **B** | 40 tests (RFC 5769 vectors, str0m, roundtrip, edge cases) | Full parser/encoder, MI (HMAC-SHA1), FP (CRC-32), ERROR-CODE. Safe byte access. |
| ICE | `nano_ice.c` | **B** | 17 tests (§7.1.1, §7.2.1, §7.3, §8, credentials) | Dual-role FSM, controlled + controlling, pacing, nomination |
| DTLS | `nano_dtls.c` | **B** | 9 tests (handshake loopback, encrypt/decrypt, keying material, fingerprint) | Sans I/O BIO adapter, ECDSA P-256 self-signed cert, RFC 5764 key export |
| SCTP-Lite | `nano_sctp.c` | **B-** | 27 tests (codec, CRC, handshake, data exchange, SACK, FORWARD-TSN, output queue) | Full codec + 4-way handshake FSM + send queue + SACK + retransmit + heartbeat + ring output queue. Missing: gap tracking, RECONFIG, SHUTDOWN-ACK. |
| DataChannel | `nano_datachannel.c` | **B** | DCEP codec + FSM tested via SCTP e2e | DCEP OPEN/ACK codec, channel management, bidirectional FSM. Idempotent OPEN handling (re-ACK on retransmit, no duplicate events). Missing: partial reliability, RECONFIG. |
| SDP | `nano_sdp.c` | **B** | 28 tests (Chrome/Firefox/Safari offers, generator, roundtrip, accept_offer, video PT, direction, IPv6 c=/o= lines) | Parser + generator with helper functions. Chrome/Firefox/Safari SDP compat. IPv6-aware `c=IN IP6 ::`/`o=` lines when local candidate is IPv6. Audio m-line with Opus/ptime:20. Video m-line: H264 PT via rtpmap+fmtp cross-validation (packetization-mode=1), prefers profile-level-id=42e01f match over first-match fallback (fixes Chrome VP8-first PT ordering). M-line ordering matches offer (RFC 8829). Direction complement (RFC 3264 §6). Codec negotiation: `remote_audio_pt` separates parsed PT from local config PT (RFC 3264 §6.1). Video PT negotiation uses fmtp-selected PT (not m-line first PT) in `rtc_apply_negotiated_media()`. Debug logging at PT selection points. |
| CRC-32c | `nano_crc32c.c` | **B** | test vector verified | 100% — Castagnoli polynomial for SCTP checksums |
| CRC-32 | `nano_crc32.c` | **B** | test vector verified | 100% — ISO HDLC polynomial for STUN FINGERPRINT |
| Address utils | `nano_addr.c` | **B** | 48 tests (IPv4/IPv6 parse, format, roundtrip, negative cases, auto-detect) | RFC 4291/5952 IPv6 parsing + formatting, IPv4 dotted-decimal. Guarded by `NANORTC_FEATURE_IPV6`. |

### Audio (AUDIO/MEDIA profiles)

| Module | File | Grade | Tests | Notes |
|--------|------|-------|-------|-------|
| RTP | `nano_rtp.c` | **B** | 8 tests (byte vectors, roundtrip, marker bit, error cases) | RFC 3550 pack/unpack, V=2/CSRC/extension header support |
| RTCP | `nano_rtcp.c` | **B** | 15 tests (SR/RR/NACK generate + parse, roundtrip, error cases) | RFC 3550 SR/RR, RFC 4585 Generic NACK, parser with validation |
| SRTP | `nano_srtp.c` | **B+** | 13 tests (RFC 3711 B.3 key derivation, RTP protect/unprotect, tamper detection, SRTCP protect/unprotect roundtrip, SRTCP tamper, SRTCP index, key direction) | RFC 3711 AES-128-CM-HMAC-SHA1-80 for both SRTP and SRTCP. Key derivation (labels 0x00-0x05). ROC tracking. SRTCP with E-flag + 31-bit index trailer. Periodic RTCP SR sending (RFC 3550 §6.2). Inbound SR handling for DLSR. |
| Jitter | `nano_jitter.c` | **B-** | 8 tests (push/pop, reorder, wraparound, playout delay, overflow) | Fixed ring buffer with playout delay and reordering |

### Video (VIDEO/MEDIA profiles)

| Module | File | Grade | Tests | Notes |
|--------|------|-------|-------|-------|
| H.264 packetizer | `nano_h264.c` | **B** | 8 tests (single NAL, FU-A fragment, reassembly, edge cases) | RFC 6184 FU-A packetizer + depacketizer/reassembly. MTU-aware fragmentation. |
| BWE | `nano_bwe.c` | **D** | — | Bandwidth estimation stub |

### Infrastructure

| Component | Grade | Notes |
|-----------|-------|-------|
| Crypto provider interface | **B** | Interface complete; HMAC-SHA1 + CSPRNG + DTLS + AES-128-CM + HMAC-SHA1-80 (both backends). DTLS-SRTP extension (RFC 5764 `use_srtp`) in both backends. mbedTLS 3-tier compat: 2.x (legacy), 3.6+ (PSA keygen + `set_serial_raw`), 4.x (full PSA). |
| Build system (CMake) | **B** | 3 profiles, 2 crypto backends, ESP-IDF detection, `-fvisibility=hidden` |
| Test infrastructure | **B** | Shared macros (`nano_test.h`), 313 tests across 13 suites, RFC 5769/3711/4291 vectors, e2e ICE+DTLS loopback, full public API coverage |
| Interop test framework | **B** | libdatachannel v0.22.5 as reference peer, 5 interop tests all pass (handshake, DC open, text/binary). SDP compat fixed (commit `4d143f2`). |
| CI pipeline | **B** | GitHub Actions: 3-profile × 2-crypto matrix, constraints, ASan. Local: `scripts/ci-check.sh` |
| Examples | **B** | Linux datachannel + media_send + browser interop (HTTP signaling + `signaling_server.py`). Browser audio+video verified: Opus → Chrome (0% concealed), H.264 → Chrome video playback. Shared `h264_utils.h` for Annex-B NAL parsing. Includes opus_verify + opus_gen_tone tools. ESP32 DataChannel example verified (WiFi + Discovery + ICE/DTLS/SCTP/DC echo, ESP32-S3). ESP32 Audio example verified (Opus sine wave → Chrome, ESP32-S3). |
| Documentation | **B** | AGENTS.md, ARCHITECTURE.md, exec plans, quality scores, core beliefs, RFC index |

## Quality Targets

| Phase | Target |
|-------|--------|
| Phase 1 complete | All core modules at **B** or above + all interop tests pass ✓ + browser & ESP32 integration verified ✓ |
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
7. ~~Browser end-to-end validation~~ **DONE** — DataChannel + Audio (Opus → Chrome) + Video (H.264 → Chrome) verified via browser_interop example
8. ~~ESP32 hardware validation~~ **DONE** — ESP32-S3 DataChannel echo verified (WiFi + UDP Discovery + ICE/DTLS/SCTP/DC, HTTP signaling via `http_signaling.c`)

**Acceptable gaps (address in later phases):**
1. No fuzz testing yet (Phase 4)
2. ~~No ESP32 hardware validation yet~~ **DONE** (see gap #8)
3. No code coverage measurement (Phase 4)
4. SCTP gap tracking / RECONFIG not yet implemented (needed for robust browser interop)
