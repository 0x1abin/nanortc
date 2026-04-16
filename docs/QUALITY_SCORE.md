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
| TURN client | `nano_turn.c` | **A** | 24 unit tests + 5 relay-only interop tests with coturn (handshake / DC string / channel-data burst / large payload / echo) | Full RFC 5766/8656: Allocate + 401 challenge + Refresh (incl. LIFETIME=0 deallocate) + CreatePermission (per-tick trickle fan-out, per-permission txid validation) + ChannelBind + Send/Data indication + ChannelData framing. Lazy outbound TURN wrap at `nanortc_poll_output()` time via `out_wrap_meta[]` side-table + dedicated `turn_buf`, gated by `via_turn` signal plumbed through `rtc_process_receive` → `ice_handle_stun`. Consent freshness routed through the same wrap path. Fuzz-tested (`fuzz_turn`). Hand-verified end-to-end with a real cellular phone via the macOS uipcat-camera SDK example (Phase 5.2). **Coverage gap:** automated test for nanortc-as-TURN-client (relay-only outbound) still pending. |
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
| H.265 packetizer | `nano_h265.c` | **B** | 51 tests (Single NAL §4.4.1, FU §4.4.3 with S/E/FuType + LayerId/TID/F-bit preservation, AP §4.4.2 with LayerId/TID min + F-bit union + greedy AU packer, keyframe detection for IRAP types 16–23, PACI §4.4.4 drop, AP/FU abort transitions) | RFC 7798 Single/AP/FU packetizer + depacketizer. Hand-crafted vectors (no reference-implementation byte copies). Fuzz harness `fuzz_h265` + 5 seed corpora ready; 50M+ target execs to reach A grade. Not yet wired to nano_rtc (PR-2). Not yet browser-verified (PR-3). |
| Annex-B scanner | `nano_annex_b.c` | **A** | 6 tests (shared via `test_h264.c`) | Codec-agnostic start-code scanner. Extracted from `nano_h264.c` to be shared with H.265. Fuzz-tested via `fuzz_h264` (31M+ execs) and `fuzz_h265`. |
| Base64 encoder | `nano_base64.c` | **A** | 12 tests (RFC 4648 §10 canonical 7 vectors + alphabet coverage + buffer overflow + NUL termination + encoded-size helper) | RFC 4648 §4 standard alphabet encoder. Single function, no decoder (not needed). Used by H.265 SDP sprop-vps/sps/pps emission. |
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

## Phase 7 Summary (Stability & Performance Hardening)

Single-session PR that fixed one latent bug and hardened the hot paths discovered by a full three-axis audit (memory / performance / stability). Every change is backward compatible; DC-only builds see zero memory impact. See [phase7-stability-performance-hardening.md](exec-plans/completed/phase7-stability-performance-hardening.md) for the full session log.

- **Critical fix — C0 (RTP receive)**: the RTP RX path used `stun_buf` (256B) as scratch, silently dropping every inbound RTP packet > 256B. `NANORTC_STUN_BUF_SIZE` now auto-enlarges to `NANORTC_MEDIA_BUF_SIZE` under `NANORTC_HAVE_MEDIA_TRANSPORT` and a `#error` assertion in `nanortc_config.h` pins the invariant so a user-provided override below `NANORTC_MEDIA_BUF_SIZE` breaks the build instead of regressing the fix.
- **SRTP hot path**: `srtp_compute_iv()` marked `static inline` (folds into the surrounding AES-CM call); `nano_srtp_t` gained `last_send_idx`/`last_recv_idx` cache slots so `srtp_get_ssrc_state()` becomes O(1) on the common BUNDLE hit path.
- **SCTP padding**: three `nsctp_encode_*` byte-loops rewritten as `memset` — authoritative single-instruction padding across every target compiler, especially xtensa-gcc.
- **Defensive integer guards**: RTP ext_len, SRTP ext_len (same logic), H.264 STAP-A sub-NAL length, and DCEP `label_len + protocol_len` all converted to subtraction-form bound checks (`a > max - b`). None of the old forms were exploitable on 32-bit size_t platforms, but the rewrite eliminates the implicit dependency on later-in-the-function length checks and lets fuzz directly exercise pathological values.
- **Poll cadence documentation**: `nanortc_handle_input()` doxygen now spells out the minimum poll interval contract; `NANORTC_MIN_POLL_INTERVAL_MS=50` added to `nanortc_config.h`.
- **Verification scope**: 19/19 ctest in default build, 93/93 across 6 feature combos × openssl, 46/46 across 3 combos × mbedtls, AddressSanitizer MEDIA build clean, clang-format clean, **768,656,267 fuzz executions (0 crashes)** across 8 harnesses, and 4/4 libdatachannel interop tests pass including audio + video (the direct end-to-end validation of the C0 fix). Cumulative fuzz budget now exceeds 1.2 billion executions on top of Phase 4's 456M base.
- **No regressions**: `test_sizeof.c` upper bounds untouched, no API breakage, DC-only builds see zero memory growth.
