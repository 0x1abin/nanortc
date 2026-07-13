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
| Main FSM | `nano_rtc.c` + `nano_rtc_negotiate.c` + `nano_rtc_media.c` | **A** | 75+ e2e tests: init, demux, ICE→DTLS→SCTP→DC pipeline, offer/answer roundtrip, output ownership/backpressure, timer wraparound, media/FEC admission, graceful close, state transitions, IPv6, parameter validation and codec variants | RFC 7983 demux, full pipeline integration, all public API. Transient control/audio producers use a per-session owned TX-slot ring; exact dequeue-cursor release keeps queued output pointers independent across bursts and across the 16-bit cursor wrap. The orchestration remains split across `nano_rtc.c`, `nano_rtc_negotiate.c`, and `nano_rtc_media.c` behind `src/nano_rtc_internal.h`. Fuzz-tested through the protocol parser harnesses. Browser + interop verified. |
| STUN codec | `nano_stun.c` | **A** | 49 tests (RFC 5769 vectors, str0m, roundtrip, edge cases, short-input guards) | Full parser/encoder, MI (HMAC-SHA1), FP (CRC-32), ERROR-CODE. Fuzz-tested (`fuzz_stun`): 76M+ executions clean. 91% line coverage. |
| ICE | `nano_ice.c` | **A** | 35+ tests (§7.1.1, §7.2.1, §7.3, §8, §6.1.2.2 pair filter, credentials, mandatory MI+FP, Binding Error, RNG failure and deadline wraparound) | Dual-role FSM, controlled + controlling, pacing, nomination, same-family pair enforcement, random tie-breaker per §5.2, SDP/STUN priority alignment. Full RFC audit: [docs/engineering/ice-rfc-compliance.md](engineering/ice-rfc-compliance.md). IPv4 + IPv6 dual-stack end-to-end (`test_e2e_ipv6_loopback_connects`). Browser + interop verified. |
| DTLS | `nano_dtls.c` | **A** | 16 tests (handshake loopback, encrypt/decrypt, output burst, keying material, fingerprint, close_notify) | Sans I/O BIO adapter, ECDSA P-256 self-signed cert, RFC 5764 key export, close_notify alert. Browser + interop verified. |
| SCTP-Lite | `nano_sctp.c` | **A** | 60 tests (codec, CRC, handshake, data exchange, SACK, FORWARD-TSN, output queue, gap tracking and RNG rollback) | Full codec + 4-way handshake FSM + send queue + SACK + retransmit + heartbeat + gap tracking. Fuzz-tested (`fuzz_sctp`): 63M+ executions clean. Browser + interop verified. |
| DataChannel | `nano_datachannel.c` | **A** | 17 unit tests + e2e (DCEP codec, channel mgmt, open/ack/idempotent, max channels, malformed input, all error paths) | DCEP OPEN/ACK codec, channel management, bidirectional FSM. Idempotent OPEN handling. Browser + interop verified. |
| SDP | `nano_sdp.c` | **A** | 45 tests (Chrome/Firefox/Safari offers, generator, roundtrip, video PT, direction parsing, IPv6, media directions) | Parser + generator. Chrome/Firefox/Safari compat. Fuzz-tested (`fuzz_sdp`): 51M+ executions clean. Browser + interop verified. |
| CRC-32c | `nano_crc32c.c` | **A** | test vector verified | 100% line coverage. Incremental API (`init/update/final`) for zero-copy SCTP checksum verification. Fuzz-tested via `fuzz_sctp`. |
| CRC-32 | `nano_crc32.c` | **A** | test vector verified | 100% line coverage. Fuzz-tested via `fuzz_stun` (called by STUN FINGERPRINT verify). |
| TURN client | `nano_turn.c` | **A** | 56 unit tests + real STUN/TURN discovery + strict relay-only coturn interop (handshake / DC string / channel-data burst / large payload / echo) | Full RFC 5766/8656 UDP profile: Allocate + bounded 401/438 auth + RFC 8489 retransmission + authenticated-response validation + Refresh (incl. LIFETIME=0 deallocate) + independently refreshed CreatePermission entries + ChannelBind keyed by peer IP/port + Send/Data indication + ChannelData framing. Relay candidates join the ICE checklist and controlling/controlled data routes by the selected local candidate. The protected external workflow uses short-lived TURN REST credentials, requires real srflx+relay discovery, and then forces a relay-to-relay topology; ordinary PR/local CI excludes network tests. Fuzz-tested (`fuzz_turn`). |
| Address utils | `nano_addr.c` | **A** | 48 tests (IPv4/IPv6 parse, format, roundtrip, negative cases, auto-detect) | RFC 4291/5952 IPv6 parsing + formatting. Fuzz-tested (`fuzz_addr`): 70M+ executions clean. 93% line coverage. |

### Media common (AUDIO or VIDEO profiles)

| Module | File | Grade | Tests | Notes |
|--------|------|-------|-------|-------|
| Track abstraction | `nano_media.c` | **A** | 23+ tests (track init, RNG-safe RTP seed commit, kind/direction/codec validation, jitter wiring, mid handling, payload-type lookups, error cases) | Shared track container used by both audio and video paths. Compiled under `NANORTC_HAVE_MEDIA_TRANSPORT`. Tests live in `tests/test_media.c`. No direct fuzz harness (data structure layer; parser coverage comes through SDP / RTP / RTCP). |

### Audio (AUDIO/MEDIA profiles)

| Module | File | Grade | Tests | Notes |
|--------|------|-------|-------|-------|
| RTP | `nano_rtp.c` | **A** | 22 tests (independent byte vectors, CSRC/extensions, zero-copy packing, TWCC, legal/illegal padding, roundtrip and error cases) | RFC 3550 pack/unpack. Padding count is validated and excluded from the exposed payload length. Fuzz-tested (`fuzz_rtp`): 83M+ executions clean. Browser verified. |
| RTCP | `nano_rtcp.c` | **A** | 26 tests (SR/RR/NACK generate + parse, roundtrip, error cases) | RFC 3550 SR/RR, RFC 4585 Generic NACK (with BLP bitmask) + PLI. Fuzz-tested via `fuzz_rtp`. PLI is emitted both on demand (`nanortc_request_keyframe`) and automatically on receive-side loss (`NANORTC_FEATURE_VIDEO_AUTO_PLI`: forward RTP seq gap → debounced PLI; `test_e2e_video_auto_pli_on_loss`). **Receiver NACK** (`NANORTC_FEATURE_VIDEO_NACK_RX`, opt-in): a forward gap emits a Generic NACK → the sender retransmits from pkt_ring → the reorder buffer fills the gap for in-order recovery (`test_e2e_video_nack_recovers_drop` proves the full loop). |
| SRTP | `nano_srtp.c` | **A** | 13 tests (RFC 3711 B.3 key derivation, RTP/SRTCP protect/unprotect, tamper detection, key direction) | RFC 3711 AES-128-CM-HMAC-SHA1-80 for SRTP + SRTCP. 85% line coverage. Browser verified. |
| Jitter | `nano_jitter.c` | **A** | 14 tests (push/pop, reorder, wraparound, playout delay, overflow, stale packet, buffer too small) | Fixed ring buffer with playout delay and reordering. 95% line coverage. Browser verified (Opus → Chrome, 0% concealed). |

### Video (VIDEO/MEDIA profiles)

| Module | File | Grade | Tests | Notes |
|--------|------|-------|-------|-------|
| H.264 packetizer | `nano_h264.c` | **A** | 40 tests (single NAL, FU-A fragment/reassembly, STAP-A, keyframe detection, Annex-B NAL finder, edge cases) | RFC 6184 FU-A packetizer + depacketizer. Fuzz-tested (`fuzz_h264`): 31M+ executions clean. Browser verified (H.264 → Chrome). |
| H.265 packetizer | `nano_h265.c` | **A** | 52 tests (Single NAL §4.4.1, FU §4.4.3 with S/E/FuType + LayerId/TID/F-bit preservation, AP §4.4.2 with LayerId/TID min + F-bit union + greedy AU packer, keyframe detection for IRAP types 16–23, PACI §4.4.4 drop, AP/FU abort transitions) | RFC 7798 Single/AP/FU packetizer + depacketizer. Hand-crafted vectors (no reference-implementation byte copies). Fuzz `fuzz_h265`: 608M+ executions clean. libdatachannel v0.22.5 interop verified (Single NAL + FU fragmentation + sprop-* on the answer). Browser smoke tracked as a follow-up validation task. |
| Annex-B scanner | `nano_annex_b.c` | **A** | 8 tests (shared via `test_h264.c`) | Codec-agnostic start-code scanner. Extracted from `nano_h264.c` to be shared with H.265. Fuzz-tested via `fuzz_h264` (31M+ execs) and `fuzz_h265`. |
| Base64 encoder | `nano_base64.c` | **A** | 12 tests (RFC 4648 §10 canonical 7 vectors + alphabet coverage + buffer overflow + NUL termination + encoded-size helper) | RFC 4648 §4 standard alphabet encoder. Single function, no decoder (not needed). Used by H.265 SDP sprop-vps/sps/pps emission. |
| BWE | `nano_bwe.c` | **A** | 43 tests (REMB parse, byte vector, EMA smoothing, runtime bounds/default expansion, initial/event clamp, TWCC loss and public API) | REMB parsing and EMA smoothing. Bounds updates validate before commit and immediately clamp the live estimate and event baseline; an initial bitrate is clamped to the expanded effective range. Fuzz-tested (`fuzz_bwe`): 82M+ executions clean. The estimate also drives the **send-side video RTP pacer** (`NANORTC_FEATURE_VIDEO_PACING`, leaky token bucket in `nano_rtc_media.c`). |
| TWCC parser | `nano_twcc.c` | **A** | 26 tests (run-length / status-vector 1-bit / status-vector 2-bit chunks, mixed chunks + delta sizes, malformed input, truncation, bad PT/FMT, count overflow) | RFC 8888 Transport-Wide Congestion Control feedback parser. Hand-built byte vectors derived from the RFC, no third-party reference bytes. A dedicated `fuzz_twcc` harness exercises chunk/delta parsing under ASan/UBSan; `fuzz_rtp` continues to cover the RTCP demux entry path. |
| FEC codec | `nano_fec.c` | **B** | 5 tests (`test_fec.c`: structural RFC-5109 field assertions, byte-exact roundtrip recovery across group sizes 1/2/4/8, variable lengths, seq wraparound, unrecoverable 0/2-loss cases) + `fuzz_fec` (ASan/UBSan clean) | Phase 13 PR-1. ULPFEC (RFC 5109 level-0) XOR encode/decode — recover any one lost packet per group of ≤16. Opt-in (`NANORTC_FEATURE_VIDEO_FEC`, default off). Codec validated to a high bar without a browser (structural field tests, not just roundtrip). CSRC out of scope; RTP header extensions ARE handled (byte-exact XOR recovery). |
| FEC send/recv | `nano_rtc_media.c` (`rtc_fec_*`) | **B** | Loopback recovery, multi-group IDR, adaptive-K, resource-pressure, H.264/H.265 and NACK-coordination e2e tests; ASan advanced-media profile | Send capture is prepare/commit: plaintext is staged first, but a group advances only after media SRTP succeeds. With pacing disabled, frame admission reserves all media packets first and emits FEC only if the output queue and dedicated FEC ring can hold every group completed by that frame; otherwise media remains atomic, all completed groups are discarded, and `stats_fec_dropped_resource` records the degradation. Exact busy masks protect the multi-group TX ring across output-cursor wrap. Receive remains wire-order-independent and suppresses NACK only when pending FEC covers the missing sequence. **B** until RED framing/SDP negotiation and browser interop land. |
| Reorder buffer | `nano_reorder.c` | **B** | 6 isolation tests (`test_reorder.c`: in-order, reorder-heal/no-loss, late-drop, timeout-skip, far-future force-advance, 16-bit wraparound) + e2e `test_e2e_video_reorder_heals_swap` (reversed pair healed in order + non-aliased, mid-stream SSRC-change not blackholed) | Phase 12 PR-2. Opt-in (`NANORTC_FEATURE_VIDEO_REORDER`, default off) bounded receive reorder buffer: releases video RTP in seq order via a poll-time one-NAL-per-poll producer (avoids depkt-buffer aliasing), precise loss signal, timer-flush latency cap. 4-finding adversarial review passed (all fixed). Exercised in CI via the `MEDIA + REORDER + NACK_RX` step + a `fuzz_reorder` harness (self-contained, adversarial push/pop with seq wraparound + non-monotonic clocks; ASan/UBSan clean). Browser-interop validation pending (B until verified on a real reordering link). |

### Infrastructure

| Component | Grade | Notes |
|-----------|-------|-------|
| Logging shim | **A** | `nano_log.c` (84 lines). Caller-injected callback (`nanortc_set_log_callback`) routed through `NANORTC_LOG{T,D,I,W,E}` macros gated by `NANORTC_LOG_LEVEL`. No separate test suite — exercised by every other test indirectly. Designed primarily as an AI-debug tap (memory: `LOG purpose`). |
| Crypto provider interface | **A** | HMAC-SHA1 + CSPRNG + DTLS + AES-128-CM + `dtls_close_notify`. DTLS-SRTP `use_srtp` in both backends. mbedTLS 3-tier compat. Browser + interop verified. |
| Build system (CMake) | **A** | 7 canonical feature profiles × 2 crypto backends, shared host/ESP-IDF video source set, explicit advanced-media profiles, fuzz/coverage builds, C99 + warning parity |
| Test infrastructure | **A** | Unity test framework (vendored), 700+ tests across 30 C suites, RFC vectors, e2e loopback, 12 auto-discovered libFuzzer harnesses, 79% CI line-coverage gate |
| Interop test framework | **A** | libdatachannel v0.22.5 reference, 8/8 video interop tests pass (5 H.264 + 3 H.265: Single NAL + FU + sprop-* fmtp) + DC + audio + 3 TURN relay scenarios |
| CI pipeline | **A** | GitHub Actions: 7 canonical profiles × 2 crypto backends; FEC without NACK; non-paced/non-adaptive FEC; full advanced H.265 + ASan; auto-discovered fuzzers with full failure-log replay; interop registration assertions; 79% line-coverage gate; ESP32-S3 advanced-media compile profile |
| Examples | **B** | Linux/macOS browser interop and ESP32 DC/audio/camera. Audio producers drain and retry the same uncommitted frame once on `NANORTC_ERR_WOULD_BLOCK`; browser audio+video verified. |
| Documentation | **A** | AGENTS.md, ARCHITECTURE.md, exec plans, quality scores, memory profiles, safe-C guide, coding standards |
| Resource optimization | **A** | Historical 34% RAM reduction, zero-copy CRC, struct padding elimination, TURN feature flag, plus profile-specific `sizeof(nanortc_t)` ceilings covering the fixed TX-slot ownership ring |

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

> *Numbers above are a Phase 4 snapshot. Subsequent phases added more graded
> modules (h265, annex_b, base64, twcc) and split `nano_rtc.c` into the
> `nano_rtc.c` + `nano_rtc_negotiate.c` + `nano_rtc_media.c` orchestration
> trio (Phase 10 PR-4); the CI matrix grew to **7-combo × 2-crypto** when
> `MEDIA_H265` was added. The per-module rows above are the live source of
> truth — see [docs/PLANS.md](PLANS.md) for the timeline.*

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
