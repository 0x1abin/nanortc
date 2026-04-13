# Phase 8: Continued Resource Optimization

**Status:** Active (planning)
**Estimated effort:** 3–5 agent sessions (split across 4 independent PRs)
**Goal:** Continue the memory/perf/stability trajectory established by Phase 6 (34% RAM reduction) and Phase 7 (hot-path + latent-bug hardening), targeting IoT / ESP32-class targets. Every sub-PR is independent and ships on its own.

## Context

Phase 7 scoped itself to a critical bug fix plus zero-risk micro-optimizations and defensive hardening so it could ship in one reviewable PR. During the same audit we identified a larger set of optimizations that need more code churn, CI time, or API commitment to land — collecting them here so future sessions can execute without re-doing the exploration.

All numbers are based on the audit snapshot taken at the end of Phase 7 against a 32-bit ARM target.

| Profile | Phase 7 baseline | Phase 8 target (aggressive overrides) |
|---|---|---|
| `CORE_ONLY` (no DC, no media) | ~13 KB | ~12 KB |
| `DC-only`, TURN off | ~27 KB | ~18 KB (−33%) |
| `DC + 1 audio track` | ~39 KB | ~28 KB (−28%) |
| `DC + audio + video`, `OUT_QUEUE=32` | ~103 KB | ~82 KB (−20%) |
| `DC + audio + video`, `OUT_QUEUE=16` | — | ~63 KB (−39%) |

## Sub-PRs

Each row is a self-contained PR. `PR-1…PR-4` are the recommended landing order. P2 entries are architectural and should only be done if profiling after P1 still shows a bottleneck.

| PR | Topic | Primary files | Risk | Expected win |
|---|---|---|---|---|
| **PR-1** | IoT memory profile: SCTP + DTLS buffer shrink | `include/nanortc_config.h`, `src/nano_dtls.h`, `src/nano_sctp.h`, `tests/test_sizeof.c`, `docs/engineering/memory-profiles.md` | Low–medium | ~9 KB per DC instance |
| **PR-2** | SCTP connection-failure event propagation | `src/nano_sctp.c`, `src/nano_rtc.c`, `include/nanortc.h` | Low | New `NANORTC_EV_DISCONNECTED` path |
| **PR-3** | Video `pkt_ring` decoupled from `OUT_QUEUE_SIZE` | `include/nanortc.h`, `include/nanortc_config.h` | Low | −19 KB at `PKT_RING=16` (VIDEO profile) |
| **PR-4** | H.264 FU-A caller-provided scratch (zero-copy) | `src/nano_h264.h`, `src/nano_h264.c`, `src/nano_rtc.c`, `tests/test_h264.c` | Medium (internal API change) | −1200 B stack + 1 memcpy/fragment |
| **P2-A** | Remove SCTP `out_bufs[]` double-buffer | `src/nano_sctp.{h,c}` | **High** (touches every state transition) | −2.4…4.8 KB + 1 memcpy/pkt |
| **P2-B** | Crypto v-table LTO / `__attribute__((hot))` | `CMakeLists.txt`, `crypto/**` | Low | ~3–5% SRTP CPU |
| **P2-C** | `nanortc_next_timeout_ms()` public API | `include/nanortc.h`, `src/nano_rtc.c` | Low (additive) | Embedded CPU idle, enables `epoll_wait` |
| **P2-D** | Mutable RX buffer zero-copy | `src/nano_rtc.c`, `include/nanortc.h` | **High (breaking API)** | 1 memcpy/RTP packet |

---

## PR-1 — IoT memory profile (SCTP + DTLS buffer shrink)

**Problem.** `NANORTC_SCTP_{SEND,RECV,RECV_GAP}_BUF_SIZE` default to 4096 and `NANORTC_DTLS_BUF_SIZE` to 2048 (× three `nano_dtls_t` buffers = 6 KB). On IoT targets with low-jitter LANs these are oversized.

**Approach.**
1. Keep default values unchanged (backward compatible).
2. Document an "IoT profile" override block in `docs/engineering/memory-profiles.md`:
    ```c
    #define NANORTC_SCTP_SEND_BUF_SIZE     2048
    #define NANORTC_SCTP_RECV_BUF_SIZE     2048
    #define NANORTC_SCTP_RECV_GAP_BUF_SIZE 2048
    #define NANORTC_SCTP_OUT_QUEUE_SIZE    2
    #define NANORTC_DTLS_BUF_SIZE          1536
    ```
3. Add a new CMake test target `tests/test_iot_profile` that compiles with the IoT overrides and runs the full DC test + interop set — locks in the smaller buffers against regressions.
4. Tighten `test_sizeof.c` upper bounds only under the IoT profile (not the default).

**Verification.** Full `ci-check.sh` (6 combo × 2 crypto) + `tests/test_iot_profile` + interop under IoT profile.

**Risk.** Reducing `recv_gap_buf` below 4 KB impacts scenarios with lots of out-of-order fragments. Must be tested against real jitter (interop test with artificial reorder).

**Expected saving.** ~9 KB per DC instance (6 KB SCTP + 3 KB DTLS).

**Rollback.** Configuration override only — revert the profile header.

---

## PR-2 — SCTP connection-failure event propagation

**Problem.** When `nsctp_handle_timeout` exceeds `NANORTC_SCTP_MAX_RETRANSMITS`, it transitions the SCTP state to `CLOSED` but does not notify the upper layer. Applications only learn about the loss on the next `nanortc_datachannel_send()` failure. This is a silent stall from the app's point of view.

**Approach.**
1. Add `bool closed_due_to_failure` to `nano_sctp_t`.
2. `nsctp_handle_timeout()` sets the flag at the same moment it transitions state to `CLOSED`.
3. After `nsctp_handle_timeout()` in `nano_rtc.c`'s timer processing, check the flag: if set and the RTC was previously `NANORTC_STATE_CONNECTED`, emit `NANORTC_EV_DISCONNECTED` (existing event type) and transition the RTC state to `NANORTC_STATE_CLOSED`.
4. Add a regression test in `tests/test_sctp.c` that stubs a non-responding peer and asserts the event fires after `NANORTC_SCTP_RTO_MAX_MS * NANORTC_SCTP_MAX_RETRANSMITS` elapsed time.

**Verification.** `test_sctp` + `test_e2e` + interop.

**Risk.** Low — purely additive. Existing apps continue to work unchanged.

**Rollback.** Revert.

---

## PR-3 — Video `pkt_ring` decoupled from `OUT_QUEUE_SIZE`

**Problem.** `nanortc_t.pkt_ring[NANORTC_OUT_QUEUE_SIZE][NANORTC_MEDIA_BUF_SIZE]` is 32 × 1232 = 38.5 KB and drives the vast majority of the full-media memory footprint. The ring serves two unrelated roles — generic output queueing and NACK retransmit window — and both are currently sized by the same macro, so tuning either direction is coarse.

**Approach.**
1. Introduce `NANORTC_VIDEO_PKT_RING_SIZE` (default = `NANORTC_OUT_QUEUE_SIZE` for backward compat; must be a power of two).
2. Retarget `pkt_ring[]` and `pkt_ring_meta[]` to the new macro under `#if NANORTC_FEATURE_VIDEO`.
3. Document IoT recommendation: `PKT_RING=16` at 30 fps = ~500 ms NACK window — enough for LAN conditions, saving 19 KB.
4. Update the NACK retransmit linear scan to use the new constant.
5. Add a `test_nack_ring` test that sends NACK requests for a mix of in-window and out-of-window SEQs and asserts no crash.

**Verification.** `test_h264`, `test_media`, interop video stream with occasional NACK injection.

**Risk.** Low — purely a size change. Existing NACK lookup code already iterates with `NANORTC_OUT_QUEUE_SIZE` and behaves the same at smaller sizes.

**Rollback.** Revert or user overrides.

---

## PR-4 — H.264 FU-A zero-copy via caller-provided scratch

**Problem.** `h264_packetize()` allocates `uint8_t pkt[NANORTC_VIDEO_MTU]` (1200 B) on the stack for every fragment and then `memcpy`s the FU-A payload into it. ESP32 video tasks run with ~4 KB stack; 1200 B is a 30% footprint and a correctness hazard. The `memcpy` is also pure overhead — the data is about to be `memcpy`d again into `pkt_ring[slot]` by the caller.

**Approach.**
1. Change `h264_packetize()` signature to accept a caller-provided `uint8_t *scratch` of at least `mtu` bytes. Pass its length alongside.
2. In `src/nano_rtc.c`, pass the corresponding `pkt_ring[slot]` buffer directly — the FU-A header is written in place and the fragment is `memcpy`d into the final send buffer once.
3. Update `tests/test_h264.c` callers to use stack-allocated scratch (test contexts have plenty of stack).
4. Update `AGENTS.md` "Where to Look" / example integration if it references the old signature.

**Verification.** `test_h264` (32 existing tests), fuzz_h264 60s, interop video.

**Risk.** Medium — breaking internal API. `h264_packetize` is not in the public `nanortc.h`, so downstream applications are unaffected, but in-tree examples and tests need to compile.

**Rollback.** Revert.

---

## P2 Series (on-demand)

### P2-A — Remove SCTP `out_bufs[]` double buffer
Refactor `nano_sctp.c` so each `nsctp_handle_*` function sets a pending-output flag rather than calling `nsctp_queue_output()`; `nsctp_poll_output()` encodes directly into the caller-provided buffer. Reference: `.local-reference/str0m/src/sctp/mod.rs` — `Sctp::poll_output`.

Saves 2.4–4.8 KB per DC instance and one `memcpy` per outbound SCTP packet. **High risk** — touches every SCTP state transition. Do only if PR-1 + PR-3 leave SCTP still dominating the profile.

### P2-B — Crypto v-table LTO / hot attribute
Enable `-flto` for Release builds and mark hot crypto provider functions `__attribute__((hot))`. Keeps the multi-provider v-table but lets LTO inline the hot path. Expected 3–5% SRTP throughput on Cortex-M.

### P2-C — `nanortc_next_timeout_ms()`
Public API returning the minimum time until the next timer (ICE check, consent freshness, DTLS retransmit, SCTP RTO, RTCP send). Lets callers replace fixed-interval poll with `epoll_wait(fd, ..., timeout)`. Purely additive.

### P2-D — Mutable RX buffer zero-copy
Change `nanortc_handle_input(rtc, const uint8_t *data, ...)` to `nanortc_handle_input(rtc, uint8_t *data, ...)` so inbound SRTP can be unprotected in place without the scratch copy. Breaking API change — requires major version bump. Defer until profiling proves the RX memcpy is a bottleneck.

---

## Acceptance Criteria (Phase 8 close)

- [ ] PR-1, PR-2, PR-3, PR-4 all merged on `develop`
- [ ] `ci-check.sh` green on 6 feature combos × 2 crypto backends + ASan + interop
- [ ] Fuzz sweep ≥60s per harness, zero crashes
- [ ] `tests/test_iot_profile` added and passing
- [ ] `docs/engineering/memory-profiles.md` documents IoT and high-video profiles
- [ ] `test_sizeof.c` upper bounds tightened where applicable
- [ ] Memory profile measurements added to `QUALITY_SCORE.md` Phase 8 summary
- [ ] P2 items either executed or explicitly deferred with rationale in the phase summary

## Non-goals

- Any change to the public `nanortc_*` API (except P2-C's additive timer API and P2-D's buffer mutability if ever pursued).
- Refactoring code paths that are already at grade A and not on a hot path.
- Introducing new dependencies (e.g., libFuzzer-unrelated external libraries).
- Touching `docs/design-docs/nanortc-design-draft.md` — the design is authoritative and Phase 8 does not change it.

## Links

- [Phase 7 exec plan (completed)](../completed/phase7-stability-performance-hardening.md)
- [Phase 6 summary (in QUALITY_SCORE.md)](../../QUALITY_SCORE.md)
- [Memory profiles](../../engineering/memory-profiles.md)
- [Technical debt tracker](../tech-debt-tracker.md)
