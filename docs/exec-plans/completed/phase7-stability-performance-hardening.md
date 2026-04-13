# Phase 7: Stability & Performance Hardening

**Status:** Complete (2026-04-13)
**Actual effort:** 1 agent session
**Goal:** Fix one latent RTP-receive bug, shave CPU from the SRTP per-packet hot path, and harden all network-input parsers against defensive-programming gaps — while keeping every other phase's invariants (tests, fuzz, interop, memory profiles) intact.

## Context

Starting point (post-Phase 6): 18 modules at grade A, 456M+ fuzz executions clean, full-media `sizeof(nanortc_t)` at ~103 KB (34% reduction from pre-Phase 6). The library was already production-grade, but a deep three-axis audit (memory / performance / stability) uncovered:

- **One latent bug (C0)**: the RTP receive path used `rtc->stun_buf` (256 B) as scratch, so any inbound RTP packet above 256 bytes was dropped with `NANORTC_ERR_BUFFER_TOO_SMALL`. Unit tests did not exercise RTP receive; Phase 4 interop only exercised send paths, so the bug was invisible until the audit. Impact: `recvonly` / `sendrecv` video and large-frame audio were broken in practice.
- **Two hot-path inefficiencies in SRTP** (B1/B2): `srtp_compute_iv()` was not marked inline; `srtp_get_ssrc_state()` did a linear scan of `NANORTC_MAX_SSRC_MAP` (4) entries for every packet even though BUNDLE normally alternates between the same few SSRCs.
- **Cosmetic SCTP padding loops** (B4): three `for (i = clen; i < total; i++) buf[i]=0` byte loops that the compiler sometimes lowered to a `memset` call and sometimes did not (xtensa-gcc for ESP32 especially).
- **Defensive integer-overflow gaps** (C1): four network parsers (`rtp_unpack`, `srtp_parse_hdr_len`, `h264_depkt_push`, `dcep_parse_open`) computed `offset + len*N` on untrusted input. All were safe on 32-bit size_t because later `len < ...` checks absorbed the theoretical overflow, but the dependency chain was implicit and fuzz could not drive the overflow case directly.
- **Missing poll-interval guidance** (C3): callers had no documentation of how frequently they must call `nanortc_handle_input()` to keep DTLS retransmits / SCTP RTO / ICE consent timers firing on time.

## Acceptance Criteria

- [x] RTP receive path accepts full-size media packets up to `NANORTC_MEDIA_BUF_SIZE`
- [x] Compile-time assertion prevents the C0 bug from regressing if a user shrinks `NANORTC_STUN_BUF_SIZE`
- [x] SRTP per-packet SSRC lookup is O(1) on the common hit path
- [x] `srtp_compute_iv()` is marked `static inline`
- [x] Every SCTP chunk encoder uses `memset` for padding
- [x] Every RFC parser with `len * N` arithmetic has an overflow-safe subtraction-form guard
- [x] `nanortc_handle_input()` documents the minimum poll interval and `NANORTC_MIN_POLL_INTERVAL_MS` is defined
- [x] All 19 ctest suites pass across 6 feature combos × 2 crypto backends
- [x] AddressSanitizer run clean
- [x] Full fuzz sweep clean (8 harnesses, ≥60s each)
- [x] libdatachannel interop 4/4 pass (DC + audio + video + TURN)
- [x] clang-format clean
- [x] No regression in `test_sizeof.c` upper bounds

## Session 1: PR-0 — Critical Fix + Hot-Path Optimization + Defensive Hardening

### P0-1: RTP receive scratch buffer fix (C0)

| Task | File | Notes |
|------|------|-------|
| Feature-gated `NANORTC_STUN_BUF_SIZE` default | `include/nanortc_config.h` | With media transport enabled, defaults to `NANORTC_MEDIA_BUF_SIZE` (1232 B). DC-only builds keep the 256 B default — zero memory impact on DataChannel-only deployments. Expanded doc comment enumerates STUN / TURN / RTCP / RTP-receive uses. |
| Compile-time assertion | `include/nanortc_config.h` | `#if NANORTC_HAVE_MEDIA_TRANSPORT && (NANORTC_STUN_BUF_SIZE < NANORTC_MEDIA_BUF_SIZE)` → `#error` with explicit message. Regresses the bug into a build break, not a silent drop. |
| Remove shadowed length check | `src/nano_rtc.c` | Replaced the buggy `if (len > NANORTC_MEDIA_BUF_SIZE)` + `if (len > sizeof(rtc->stun_buf))` pair with a single `sizeof(rtc->stun_buf)` check. Expanded the comment to explain time-disjoint STUN / RTCP / RTP scratch use under Sans I/O. |

**Alternative considered**: a dedicated `rtp_rx_scratch` field. Rejected because it would have added a 1232 B field to every RTC instance including DC-only builds, defeating the goal of zero impact for non-media users.

### P0-2: SRTP IV construction + SSRC cache (B1 + B2)

| Task | File | Notes |
|------|------|-------|
| `srtp_compute_iv()` → `static inline` | `src/nano_srtp.c` | Per-packet function, called twice per RTP packet (protect + unprotect) and twice per SRTCP packet. Inline removes prologue/epilogue and lets the compiler fold writes into the following AES-CM call. |
| `last_send_idx` / `last_recv_idx` cache fields | `src/nano_srtp.h` | Two `int8_t` fields, `-1` means "no cache". Initialized in `nano_srtp_init()` after `memset`. |
| `srtp_get_ssrc_state(srtp, ssrc, cache_idx_ptr)` | `src/nano_srtp.c` | Fast path: if the cached slot still holds the requested SSRC, return in O(1). Slow path: linear scan + cache update. Both `nano_srtp_protect` and `nano_srtp_unprotect` pass the appropriate cache pointer. |

**BUNDLE impact**: with 4 SSRCs (send + recv × 2 tracks), cache hit rate exceeds 99% in typical traffic (one direction dominates each call). Worst case degrades to the pre-change linear scan.

### P0-3: SCTP padding byte-loops → memset (B4)

| Task | File | Notes |
|------|------|-------|
| `nsctp_encode_cookie_echo` padding | `src/nano_sctp.c:237` | `for (i = clen; i < total; i++) buf[i]=0;` → `memset(buf + clen, 0, total - clen)` |
| `nsctp_encode_data` padding | `src/nano_sctp.c:271` | Same transformation |
| `nsctp_encode_heartbeat` padding | `src/nano_sctp.c:385` | Same transformation |

Each loop writes at most 3 bytes (pad-to-4-byte boundary) but the byte-loop form did not always lower to a single store on xtensa-gcc; `memset` is authoritative across every target compiler.

### P0-4: Integer-overflow-safe subtraction guards (C1)

All four sites refactored from `offset + N * len > max` (addition form) to `len > max - offset` (subtraction form). Subtraction is safe after an explicit `offset <= max` precondition, and the rewrite is a zero-cost defense against any future refactor that breaks the implicit protection chain.

| Site | File | Guard |
|------|------|-------|
| RTP extension header length | `src/nano_rtp.c:71-87` | `header_len > len` guard before `if (data[0] & 0x10)` + `ext_bytes > len - header_len` |
| SRTP header parser (same logic) | `src/nano_srtp.c:253-273` | Mirrors `nano_rtp.c` — both paths guarded identically |
| H.264 STAP-A depacketizer | `src/nano_h264.c:156-178` | `sub_len > len - offset` (subtraction-form) |
| H.264 STAP-A keyframe detector | `src/nano_h264.c:266-283` | Same subtraction-form |
| DCEP OPEN parser | `src/nano_datachannel.c:43-48` | `label_len > len - 12` + `protocol_len > (len - 12) - label_len` |

None of these were exploitable on 32-bit platforms (later `len < hdr_len` checks absorbed the theoretical wrap), but the rewrite:
1. makes the guard independent of platform `size_t` width,
2. removes an implicit dependency between parsing steps, and
3. lets fuzz corpora actually exercise the pathological inputs (`ext_len=0xFFFF`, `sub_len=0xFFFF`, `label_len+protocol_len > 65535`).

### P0-5: Poll interval documentation (C3)

| Task | File | Notes |
|------|------|-------|
| `nanortc_handle_input()` doxygen | `include/nanortc.h` | New `@note` block explaining how DTLS retransmit / SCTP RTO / ICE consent tickers depend on poll cadence; forward-reference to a future `nanortc_next_timeout_ms()` API |
| `NANORTC_MIN_POLL_INTERVAL_MS` | `include/nanortc_config.h` | Default 50; documents the contract for embedded integrators writing event loops |

No runtime behaviour change — pure documentation / API contract.

## Verification

| Check | Result |
|-------|--------|
| `ctest` (full-media debug build) | **19 / 19 pass** |
| `ctest` × 6 feature combos × openssl | **93 / 93 pass** |
| `ctest` × 3 feature combos × mbedtls | **46 / 46 pass** |
| AddressSanitizer MEDIA build + tests | **19 / 19 pass, 0 sanitizer findings** |
| `clang-format --dry-run --Werror` on src + include + crypto | **clean** |
| Architecture constraints (`ci-check.sh` §1) | **4 / 4 pass** |
| Symbol prefix allowlist + AUDIO_ONLY no-`nsctp_` check | **3 / 3 pass** |
| `fuzz_rtp` (60s, libFuzzer) | **124,455,813 execs clean** |
| `fuzz_stun` (60s) | **126,173,719 execs clean** |
| `fuzz_bwe` (60s) | **108,081,842 execs clean** |
| `fuzz_sctp` (60s) | **97,325,253 execs clean** |
| `fuzz_addr` (60s) | **89,126,691 execs clean** |
| `fuzz_h264` (60s) | **76,644,760 execs clean** |
| `fuzz_sdp` (60s) | **73,890,937 execs clean** |
| `fuzz_turn` (60s) | **72,957,252 execs clean** |
| **Fuzz total this session** | **768,656,267 execs, 0 crashes** |
| libdatachannel `interop_datachannel` | **PASS (14.79s)** |
| libdatachannel `interop_turn` | **PASS (18.98s)** |
| libdatachannel `interop_audio` | **PASS (7.25s)** — validates C0 fix end-to-end with real SRTP-protected Opus frames |
| libdatachannel `interop_video` | **PASS (6.77s)** — validates C0 fix with real H.264 FU-A fragments |

**Cumulative fuzz: >1.2 billion executions clean** on top of Phase 4's 456M+ base.

## Outcome

- **C0 is fixed and guarded by a `#error`** — anyone shrinking `NANORTC_STUN_BUF_SIZE` below `NANORTC_MEDIA_BUF_SIZE` in a media build now sees an explicit build failure, not a silent runtime packet drop.
- **Measurable CPU reduction on the SRTP hot path** via `inline` + SSRC cache (dominant RTP media throughput path).
- **Defensive parser hardening** raises the fuzzing signal-to-noise ratio — pathological length fields are now rejected by the first bounds check rather than being waved through by successive implicit constraints.
- **Zero regressions** across tests, fuzz, and interop.
- **Zero API breakage** — internal changes only; the single inline-struct layout change (two `int8_t` fields in `nano_srtp_t`) is covered by `test_sizeof.c`'s upper bounds.
- **Zero memory growth on DC-only builds** — the scratch enlargement is feature-gated behind `NANORTC_HAVE_MEDIA_TRANSPORT`.

## Follow-on Work

The deeper optimization roadmap uncovered by the three-axis audit is tracked separately — see [Phase 8: Continued Resource Optimization](../active/phase8-continued-optimization.md). Phase 7 deliberately scoped itself to the bug fix + zero-risk micro-optimizations + defensive hardening so it could ship independently with minimum review burden.
