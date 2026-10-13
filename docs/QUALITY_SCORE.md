# Quality Score

Per-module quality grades for NanoRTC. Updated as implementation progresses.

0.2.0 changes authentication, negotiation, output completion, lifecycle,
statistics and congestion control. Current acceptance is tracked in
[nanortc-02.md](engineering/nanortc-02.md). Targeted auth/negotiation suites pass
on host OpenSSL and mbedTLS. Fresh Chrome 147 verification passed four sessions
(both crypto providers, both negotiation roles): 18 DC echoes per session,
Opus reception and H.264 decode. H.265 browser decode was not exercised.
The controlled-link phase retains reliable DC/audio and resumes decoded H.264
after capacity returns. Browser estimates remain REMB-capped; fixed encoded
samples do not establish encoder adaptation speed. Four independent-peer
interop suites pass, including three extra consecutive video repetitions.
ESP-IDF 6.1 builds passed four example configurations and seven P4 profiles;
all original host memory ceilings pass. CPU results are scoped in the
[benchmark record](../benchmarks/README.md). All-advanced ASan/UBSan builds
pass 43/43 suites on each of OpenSSL and mbedTLS. Twelve fuzz harnesses completed 14 runs and 96,285,341
executions without failures. Deterministic 50/100/200 ms feedback simulations
meet the 2 s decrease and 15 s recovery gates, with receiver-cap and unsupported
probe-growth regressions. Final full CI passes 54/54 checks, covering both
providers across seven profiles and feature/ownership configurations. All four
ESP-IDF example applications and seven P4 profiles were rebuilt successfully.
Eight invalid policy configurations are rejected at compile time. External TURN
deployment, hardware runtime and LeakSanitizer are not claimed.

Dated browser, fuzz, coverage and test-count figures below describe prior
revisions unless a row explicitly identifies 0.2.0. Historical **A** grades
are not a blanket certification of the changed ABI. The 2026-09-07 records remain
in [design-hardening.md](engineering/design-hardening.md) and
[datachannel-zero-retransmit.md](engineering/datachannel-zero-retransmit.md).

## Grading Scale

| Grade | Meaning |
|-------|---------|
| **A** | Complete, tested with real browser, fuzz-tested, reviewed |
| **B** | Implemented within the stated protocol subset, host-tested; live validation may be pending |
| **B-** | Implemented and tested, but missing some edge cases or interop testing |
| **C** | Partially implemented, basic tests pass |
| **D** | Stub only — compiles but returns NOT_IMPLEMENTED |
| **—** | Not applicable for current profile |

## Module Grades

### Core (all profiles)

| Module | File | Grade | Tests | Notes |
|--------|------|-------|-------|-------|
| Main FSM | `nano_rtc.c` + `nano_rtc_negotiate.c` + `nano_rtc_media.c` | **B** | 0.2: e2e, negotiation, output-lifetime, stats and established-session restart suites; advanced ASan/UBSan 43/43 | One outstanding transmit token; explicit SENT/DROPPED commit; bounded payload ownership and coalesced scalar notifications. Disconnect is immediate. Final matrix/live evidence is tracked in the 0.2 record. |
| STUN codec | `nano_stun.c` | **A** | 49 tests (RFC 5769 vectors, str0m, roundtrip, edge cases, short-input guards) | Full parser/encoder, MI (HMAC-SHA1), FP (CRC-32), ERROR-CODE. Fuzz-tested (`fuzz_stun`): 76M+ executions clean. 91% line coverage. |
| ICE | `nano_ice.c` | **B** | 41 ICE regressions plus deterministic NAT/DC tests in both roles; retained dual-stack, restart, backpressure and interop checks | Shared send/deadline selection, authenticated triggered checks and remote prflx admission, same-ID retransmission, regular/deferred nomination and srflx base routing. Bounded subset: full checklist ordering, local prflx expansion and role repair remain incomplete. [Implementation and validation](engineering/issue-81-ice.md). |
| DTLS | `nano_dtls.c` | **B** | 0.2: missing/wrong fingerprint, required provider callback, real handshake, timer and data tests | Peer SHA-256 digest must match SDP before ESTABLISHED/key export. OpenSSL and mbedTLS targeted suites pass. Four fresh Chrome role/provider sessions also pass; malformed identity coverage comes from the DTLS tests. |
| SCTP-Lite | `nano_sctp.c` | **B-** | 68 tests (codec, capacity canaries, atomic fragmentation, owned bundled delivery, iterative small-fragment reassembly, stream identity/reserved wire bits, PR-SCTP Forward TSN, gap tracking and RNG rollback) | Bounded fragmentation/reassembly, cumulative SACK, retry-limited messages and Forward TSN. Congestion control, gap-SACK fast retransmit and complete graceful shutdown remain incomplete; this is not a full RFC 9260 implementation. Fuzz-tested (`fuzz_sctp`): 63M+ executions clean. Browser + interop verified. |
| DataChannel | `nano_datachannel.c` | **B** | DCEP/channel and e2e suites; 0.2 lifecycle regression | Bounded DCEP/SCTP delivery and explicit retry policy. Individual close returns NOT_IMPLEMENTED; RFC 6525 stream reset and complete graceful SCTP shutdown are not advertised. |
| SDP | `nano_sdp.c` | **B** | 0.2: bounded parsing, failure snapshots, pending offers, priority/related-address persistence and real-DTLS ICE restart tests | Remote-only staging, explicit lengths, binary candidates and strict SHA-256 identity. General renegotiation is unsupported; existing association, role and media must survive pure ICE restart. |
| CRC-32c | `nano_crc32c.c` | **A** | test vector verified | 100% line coverage. Incremental API (`init/update/final`) for zero-copy SCTP checksum verification. Fuzz-tested via `fuzz_sctp`. |
| CRC-32 | `nano_crc32.c` | **A** | test vector verified | 100% line coverage. Fuzz-tested via `fuzz_stun` (called by STUN FINGERPRINT verify). |
| TURN client | `nano_turn.c` | **B** | 63 unit tests, including refresh scheduling, unsigned errors, concurrent nonce rotation, late replies and bounded 438; historical external discovery/relay-only interop | UDP TURN subset with legacy MD5/HMAC-SHA1 credentials: Allocate + bounded 401/438 auth + RFC 8489 retransmission + authenticated-response validation + Refresh (incl. LIFETIME=0 deallocate) + bounded CreatePermission scheduling with peer-scoped error isolation + ChannelBind keyed by peer IP/port + Send/Data indication + ChannelData framing. Relay candidates join the ICE checklist and controlling/controlled data routes by the selected local candidate. The protected external workflow uses short-lived TURN REST credentials, requires real srflx+relay discovery, and then forces a relay-to-relay topology; ordinary PR/local CI excludes network tests. Output/deadline selection and peer readiness are module-owned. Nonce rotation changes transaction IDs without resetting retry budgets. SHA-256, PASSWORD-ALGORITHMS and USERHASH are not implemented; external TURN was not rerun in this revision. Fuzz-tested (`fuzz_turn`). |
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
| BWE | `nano_bwe.c` | **B** | 0.2: 50/100/200 ms weak-network simulations meet 2 s decrease / 15 s recovery gates; receiver caps, wraps, history and six probe-gate modes | Actual socket-send history, fixed-time loss control, delay/load/stability gates on upward probing. Not a complete GCC delay model. Historical EMA/fuzz results do not verify the new controller. |
| TWCC parser | `nano_twcc.c` | **B** | Draft-holmer byte vectors, malformed/truncated chunk/delta tests and callback atomicity | Transport-wide-cc PT=205/FMT=15 parser; not RFC 8888 CCFB. Fresh instrumented `fuzz_twcc` run passes; aggregate 0.2 fuzz evidence is above. |
| FEC codec | `nano_fec.c` | **B** | 10 codec tests: RFC field vectors, variable lengths/wrap, malformed members, duplicates and cross-SSRC isolation; `fuzz_fec` sanitizer runs | RFC 5109 level-0 recovery, one loss per group. Opt-in by default. Shared sender groups reset safely on SSRC change; ring/receive coverage is bounded. Current browser runs do not validate FEC negotiation or recovery. |
| FEC send/recv | `nano_rtc_media.c` (`rtc_fec_*`) | **B** | Loopback recovery, multi-group IDR, adaptive-K, resource-pressure, H.264/H.265 and NACK-coordination e2e tests; ASan advanced-media profile | Send capture is prepare/commit: plaintext is staged first, but a group advances only after media SRTP succeeds. With pacing disabled, frame admission reserves all media packets first and emits FEC only if the output queue and dedicated FEC ring can hold every group completed by that frame; otherwise media remains atomic, all completed groups are discarded, and `stats_fec_dropped_resource` records the degradation. Exact busy masks protect the multi-group TX ring across output-cursor wrap. Receive remains wire-order-independent and suppresses NACK only when pending FEC covers the missing sequence. **B** until RED framing/SDP negotiation and browser interop land. |
| Reorder buffer | `nano_reorder.c` | **B** | 6 isolation tests (`test_reorder.c`: in-order, reorder-heal/no-loss, late-drop, timeout-skip, far-future force-advance, 16-bit wraparound) + e2e `test_e2e_video_reorder_heals_swap` (reversed pair healed in order + non-aliased, mid-stream SSRC-change not blackholed) | Phase 12 PR-2. Opt-in (`NANORTC_FEATURE_VIDEO_REORDER`, default off) bounded receive reorder buffer: releases video RTP in seq order via a poll-time one-NAL-per-poll producer (avoids depkt-buffer aliasing), precise loss signal, timer-flush latency cap. 4-finding adversarial review passed (all fixed). Exercised in CI via the `MEDIA + REORDER + NACK_RX` step + a `fuzz_reorder` harness (self-contained, adversarial push/pop with seq wraparound + non-monotonic clocks; ASan/UBSan clean). Browser-interop validation pending (B until verified on a real reordering link). |

### Infrastructure

| Component | Grade | Notes |
|-----------|-------|-------|
| Logging shim | **A** | Instance-owned `nanortc_config_t.log` passed explicitly through `NANORTC_LOG{T,D,I,W,E}` macros gated by `NANORTC_LOG_LEVEL`. No mutable global logger or ambient parser logging. Two-instance isolation is covered by E2E regression tests. |
| Crypto provider interface | **B** | 0.2 requires peer SHA-256 accessor in addition to existing time/deadline hooks. mbedTLS caches the leaf digest during verification; local IDF 6.1 / mbedTLS 4.1 headers checked, final embedded builds tracked separately. |
| Build system (CMake) | **A** | 7 canonical feature profiles × 2 crypto backends, shared host/ESP-IDF video source set, explicit advanced-media profiles, fuzz/coverage builds, C99 + warning parity |
| Test infrastructure | **A** | Unity test framework (vendored), 700+ tests across 30 C suites, RFC vectors, e2e loopback, 12 auto-discovered libFuzzer harnesses, 79% CI line-coverage gate |
| Interop test framework | **A** | libdatachannel v0.22.5 reference, 8/8 video interop tests pass (5 H.264 + 3 H.265: Single NAL + FU + sprop-* fmtp) + DC + audio + 3 TURN relay scenarios |
| CI pipeline | **A** | GitHub Actions: 7 canonical profiles × 2 crypto backends; FEC without NACK; non-paced/non-adaptive FEC; full advanced H.265 + ASan; auto-discovered fuzzers with full failure-log replay; interop registration assertions; 79% line-coverage gate; ESP32-S3 advanced-media compile profile |
| Examples | **B** | 0.2 shared UDP helper retains at most one copied datagram/token on EAGAIN, reports final result and permits input/timer progress. Four ESP example builds and four Chrome sessions passed; hardware runtime remains untested. |
| Documentation | **A** | AGENTS.md, ARCHITECTURE.md, exec plans, quality scores, memory profiles, safe-C guide, coding standards |
| Resource optimization | **A** | Historical 34% RAM reduction, zero-copy CRC, struct padding elimination, TURN feature flag, plus profile-specific `sizeof(nanortc_t)` ceilings covering the fixed TX-slot ownership ring |

## Quality Targets

| Phase | Target |
|-------|--------|
| Phase 1 complete | All core modules at **B** or above + all interop tests pass ✓ + browser & ESP32 integration verified ✓ |
| Phase 2 complete | All audio modules at **B** or above ✓ |
| Phase 3 complete | All modules at **B** or above ✓ |
| Phase 4 complete | All modules at **A** ✓ (fuzz-tested, browser-verified, interop-verified, 80%+ coverage) |

## Example I/O validation

`test_example_io` and `test_example_io_fcntl` inject allocator/socket failures
into the actual shared example helpers. They cover missing interface enumeration,
nonblocking receive races and interrupted waits, timer progression, IPv4 socket
conversion in dual-stack builds, and bounded HTTP buffer cleanup. This native
coverage does not certify WASI runtime behavior. See the
[Issue #81 example record](engineering/issue-81-examples.md).

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
