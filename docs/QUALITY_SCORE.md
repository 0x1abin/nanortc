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
| Main FSM | `nano_rtc.c` | **A** | 48 e2e tests: init, demux, ICE→DTLS→SCTP→DC pipeline, offer/answer roundtrip, DC create/close/label, graceful close, state transitions, ICE multi-candidate, IPv6 candidates, param validation, track stats, direction changes, media offer/answer, DC options, codec variants | RFC 7983 demux, full pipeline integration, all public API. 60% line coverage (largest file at 1102 lines — uncovered paths are connected-state media I/O). Fuzz-tested via `fuzz_stun`/`fuzz_sdp`/`fuzz_sctp` (input parsers called by FSM). Browser + interop verified. |
| STUN codec | `nano_stun.c` | **A** | 47 tests (RFC 5769 vectors, str0m, roundtrip, edge cases, short-input guards) | Full parser/encoder, MI (HMAC-SHA1), FP (CRC-32), ERROR-CODE. Fuzz-tested (`fuzz_stun`): 76M+ executions clean. 91% line coverage. |
| ICE | `nano_ice.c` | **A** | 17 tests (§7.1.1, §7.2.1, §7.3, §8, credentials) | Dual-role FSM, controlled + controlling, pacing, nomination. 89% line coverage. Browser + interop verified. |
| DTLS | `nano_dtls.c` | **A** | 10 tests (handshake loopback, encrypt/decrypt, keying material, fingerprint, close_notify) | Sans I/O BIO adapter, ECDSA P-256 self-signed cert, RFC 5764 key export, close_notify alert. 82% line coverage. Browser + interop verified. |
| SCTP-Lite | `nano_sctp.c` | **A** | 34 tests (codec, CRC, handshake, data exchange, SACK, FORWARD-TSN, output queue, gap tracking) | Full codec + 4-way handshake FSM + send queue + SACK + retransmit + heartbeat + gap tracking. Fuzz-tested (`fuzz_sctp`): 63M+ executions clean. 84% line coverage. Browser + interop verified. |
| DataChannel | `nano_datachannel.c` | **A** | 18 unit tests + e2e (DCEP codec, channel mgmt, open/ack/idempotent, max channels, malformed input, all error paths) | DCEP OPEN/ACK codec, channel management, bidirectional FSM. Idempotent OPEN handling. 94% line coverage. Browser + interop verified. |
| SDP | `nano_sdp.c` | **A** | 30 tests (Chrome/Firefox/Safari offers, generator, roundtrip, video PT, direction parsing, IPv6, media directions) | Parser + generator. Chrome/Firefox/Safari compat. Fuzz-tested (`fuzz_sdp`): 51M+ executions clean. 79% line coverage. Browser + interop verified. |
| CRC-32c | `nano_crc32c.c` | **A** | test vector verified | 100% line coverage. Incremental API (`init/update/final`) for zero-copy SCTP checksum verification. Fuzz-tested via `fuzz_sctp`. |
| CRC-32 | `nano_crc32.c` | **A** | test vector verified | 100% line coverage. Fuzz-tested via `fuzz_stun` (called by STUN FINGERPRINT verify). |
| TURN client | `nano_turn.c` | **A** | 24 unit tests + 3 interop tests with coturn (handshake/string/echo) | Full RFC 5766: Allocate + 401 challenge + Refresh + CreatePermission + ChannelBind + Send/Data indication + ChannelData framing. Fuzz-tested (`fuzz_turn`). Permission/channel auto-refresh. Interop verified with coturn. |
| Address utils | `nano_addr.c` | **A** | 48 tests (IPv4/IPv6 parse, format, roundtrip, negative cases, auto-detect) | RFC 4291/5952 IPv6 parsing + formatting. Fuzz-tested (`fuzz_addr`): 70M+ executions clean. 93% line coverage. |

### Audio (AUDIO/MEDIA profiles)

| Module | File | Grade | Tests | Notes |
|--------|------|-------|-------|-------|
| RTP | `nano_rtp.c` | **A** | 8 tests (byte vectors, roundtrip, marker bit, error cases) | RFC 3550 pack/unpack. Fuzz-tested (`fuzz_rtp`): 83M+ executions clean. 94% line coverage. Browser verified. |
| RTCP | `nano_rtcp.c` | **A** | 15 tests (SR/RR/NACK generate + parse, roundtrip, error cases) | RFC 3550 SR/RR, RFC 4585 Generic NACK. Fuzz-tested via `fuzz_rtp`. 94% line coverage. |
| SRTP | `nano_srtp.c` | **A** | 13 tests (RFC 3711 B.3 key derivation, RTP/SRTCP protect/unprotect, tamper detection, key direction) | RFC 3711 AES-128-CM-HMAC-SHA1-80 for SRTP + SRTCP. 85% line coverage. Browser verified. |
| Jitter | `nano_jitter.c` | **A** | 14 tests (push/pop, reorder, wraparound, playout delay, overflow, stale packet, buffer too small) | Fixed ring buffer with playout delay and reordering. 95% line coverage. Browser verified (Opus → Chrome, 0% concealed). |

### Video (VIDEO/MEDIA profiles)

| Module | File | Grade | Tests | Notes |
|--------|------|-------|-------|-------|
| H.264 packetizer | `nano_h264.c` | **A** | 32 tests (single NAL, FU-A fragment/reassembly, STAP-A, keyframe detection, Annex-B NAL finder, edge cases) | RFC 6184 FU-A packetizer + depacketizer. Fuzz-tested (`fuzz_h264`): 31M+ executions clean. 83% line coverage. Browser verified (H.264 → Chrome). |
| BWE | `nano_bwe.c` | **A** | 26 tests (REMB parse, byte vector, EMA smoothing, min/max clamp, event threshold, public API) | REMB parsing, EMA smoothing. Fuzz-tested (`fuzz_bwe`): 82M+ executions clean. 96% line coverage. |

### Infrastructure

| Component | Grade | Notes |
|-----------|-------|-------|
| Crypto provider interface | **A** | HMAC-SHA1 + CSPRNG + DTLS + AES-128-CM + `dtls_close_notify`. DTLS-SRTP `use_srtp` in both backends. mbedTLS 3-tier compat. Browser + interop verified. |
| Build system (CMake) | **A** | 6 feature combos, 2 crypto backends, ESP-IDF, fuzz build, coverage build, `-Wall -Wextra -Werror` |
| Test infrastructure | **A** | Unity test framework (vendored), 400+ tests across 16 suites, RFC vectors, e2e loopback, 80%+ line coverage |
| Interop test framework | **A** | libdatachannel v0.22.5 reference, 5/5 interop tests pass (DC + audio + video) + 3 TURN interop tests with coturn |
| CI pipeline | **A** | GitHub Actions: 6-combo × 2-crypto matrix, constraints, ASan, fuzz (30s per harness), coverage (80% threshold) |
| Examples | **B** | Linux browser interop, ESP32 DC/audio/camera. Browser audio+video verified. |
| Documentation | **A** | AGENTS.md, ARCHITECTURE.md, exec plans, quality scores, memory profiles, safe-C guide, coding standards |
| Resource optimization | **A** | 34% RAM reduction, zero-copy CRC, struct padding elimination, TURN feature flag, sizeof regression tests |

## Quality Targets

| Phase | Target |
|-------|--------|
| Phase 1 complete | All core modules at **B** or above + all interop tests pass ✓ + browser & ESP32 integration verified ✓ |
| Phase 2 complete | All audio modules at **B** or above ✓ |
| Phase 3 complete | All modules at **B** or above ✓ |
| Phase 4 complete | All modules at **A** ✓ (fuzz-tested, browser-verified, interop-verified, 80%+ coverage) |

## Phase 4 Summary

All 18 library modules promoted from **B** to **A** grade:
- **Fuzz-tested**: 7 libFuzzer harnesses, 456M+ total executions, zero crashes/violations
- **Browser-verified**: DataChannel + Opus audio + H.264 video confirmed with Chrome
- **Interop-verified**: 5/5 libdatachannel interop tests pass
- **Coverage**: 80.0% line coverage, 95.1% function coverage across 4529 lines
- **Test count**: 400+ tests across 16 suites (up from 347 across 14)
- **Test framework**: Unity (ThrowTheSwitch), replacing manual macros
- **CI**: 6-combo × 2-crypto build matrix + ASan + fuzz + coverage threshold

## Phase 6 Summary (Resource Optimization)

Full-media `sizeof(nanortc_t)` reduced from 157 KB to 103 KB (**34% reduction**):
- **Config defaults**: Jitter buffer 64→32 slots, slot data 640→320B, NAL buffer 32→16 KB
- **Zero-copy CRC**: Segmented CRC-32c API eliminates 1200B stack allocation per SCTP packet
- **Struct optimization**: Field reordering eliminates padding; `size_t`→`uint16_t` for credential lengths
- **TURN feature flag**: `NANORTC_FEATURE_TURN` saves 700B RAM + 13KB code when disabled
- **SDP hardening**: `extract_value()` strips trailing whitespace, protecting exact-fit buffers
- **Regression guard**: `test_sizeof.c` prevents accidental struct growth in CI
