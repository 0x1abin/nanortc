# Phase 8: Continued Optimization & Stability

**Status:** Active (planning)
**Estimated effort:** 4–6 agent sessions (split across 5 independent PRs)
**Goal:** Continue the memory/perf/stability trajectory established by Phase 6 (34% RAM reduction) and Phase 7 (hot-path + latent-bug hardening), targeting IoT / ESP32-class targets. Also close out the ICE CONTROLLING bug uncovered by Phase 7 browser_interop testing. Every sub-PR is independent and ships on its own.

## Context

Phase 7 scoped itself to a critical bug fix plus zero-risk micro-optimizations and defensive hardening so it could ship in one reviewable PR. During the same audit we identified a larger set of optimizations that need more code churn, CI time, or API commitment to land — collecting them here so future sessions can execute without re-doing the exploration.

In addition, while running Phase 7's `examples/browser_interop` end-to-end tests against headless Chrome 146 (WSL2 mirrored networking), we observed that `nanortc --offer` against a Chrome answerer gets stuck in `ICE_STATE_CHECKING` even though the browser briefly observes `iceConnectionState: "connected"`. Diagnosis pinned it to a pre-existing bug in `nano_ice.c` where the CONTROLLING role overwrites its single `last_txid` / `last_{local,remote}_idx` scratch every 50ms while pacing checks, making all but the most recent Binding Response unmatchable. The answer-mode tests (T1 / T3 / T4) are unaffected because CONTROLLED role only responds to inbound requests and never matches its own outstanding transactions. PR-5 tracks the fix for this.

All memory numbers below are based on the audit snapshot taken at the end of Phase 7 against a 32-bit ARM target.

| Profile | Phase 7 baseline | Phase 8 target (aggressive overrides) |
|---|---|---|
| `CORE_ONLY` (no DC, no media) | ~13 KB | ~12 KB |
| `DC-only`, TURN off | ~27 KB | ~18 KB (−33%) |
| `DC + 1 audio track` | ~39 KB | ~28 KB (−28%) |
| `DC + audio + video`, `OUT_QUEUE=32` | ~103 KB | ~82 KB (−20%) |
| `DC + audio + video`, `OUT_QUEUE=16` | — | ~63 KB (−39%) |

## Sub-PRs

Each row is a self-contained PR. `PR-1…PR-5` are the recommended landing order. P2 entries are architectural and should only be done if profiling after P1 still shows a bottleneck.

| PR | Topic | Primary files | Risk | Expected win |
|---|---|---|---|---|
| **PR-1** | IoT memory profile: SCTP + DTLS buffer shrink | `include/nanortc_config.h`, `src/nano_dtls.h`, `src/nano_sctp.h`, `tests/test_sizeof.c`, `docs/engineering/memory-profiles.md` | Low–medium | ~9 KB per DC instance |
| **PR-2** | SCTP connection-failure event propagation | `src/nano_sctp.c`, `src/nano_rtc.c`, `include/nanortc.h` | Low | New `NANORTC_EV_DISCONNECTED` path |
| **PR-3** | Video `pkt_ring` decoupled from `OUT_QUEUE_SIZE` | `include/nanortc.h`, `include/nanortc_config.h` | Low | −19 KB at `PKT_RING=16` (VIDEO profile) |
| **PR-4** | H.264 FU-A caller-provided scratch (zero-copy) | `src/nano_h264.h`, `src/nano_h264.c`, `src/nano_rtc.c`, `tests/test_h264.c` | Medium (internal API change) | −1200 B stack + 1 memcpy/fragment |
| **PR-5** | ICE CONTROLLING: per-pair pending transaction table | `src/nano_ice.h`, `src/nano_ice.c`, `tests/test_ice.c`, `examples/browser_interop` | Medium (state machine edit) | Unblocks nanortc-as-offerer against real browsers |
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

## PR-5 — ICE CONTROLLING per-pair pending transaction table

**Problem.** The CONTROLLING role in `src/nano_ice.c` only keeps a single scratch triple for its in-flight connectivity check:

- `ice->last_txid` — the STUN transaction ID of the most recently sent Binding Request (`ice_generate_check`, nano_ice.c:284–285)
- `ice->last_local_idx` / `ice->last_remote_idx` — the pair index that check was sent to (nano_ice.c:318–319)

`rtc_process_timers` calls `ice_generate_check` every `NANORTC_ICE_CHECK_INTERVAL_MS` (50 ms), each call overwrites all three fields with fresh random transaction bytes and advances to the next pair. When a Binding Response arrives, `ice_handle_stun` matches it against the single saved `last_txid` (nano_ice.c:197–199):

```c
if (memcmp(msg.transaction_id, ice->last_txid, STUN_TXID_SIZE) != 0) {
    return NANORTC_ERR_PROTOCOL;
}
```

So all but the most-recently-sent check's response is rejected with `NANORTC_ERR_PROTOCOL`. In a typical browser scenario with 1–2 remote host candidates plus a srflx candidate, nanortc will send 2–3 checks within 100 ms, the browser responds quickly, but only one response has any chance of matching — and even then, if the caller hasn't drained the UDP socket in under 50 ms, every response is stale.

**Evidence.** Discovered while running `examples/browser_interop` T2 (nanortc --offer against Chrome 146 headless answerer) during Phase 7 validation:

- nanortc side: never emits `NANORTC_EV_ICE_STATE_CHANGE(CONNECTED)`, stays in `ICE_STATE_CHECKING` until `check_count == NANORTC_ICE_MAX_CHECKS` and silently transitions to FAILED (no user event for that state today — see secondary concern below).
- Chrome side: ICE connection state briefly pulses `checking → connected → disconnected` over ~15 seconds because Chrome (CONTROLLED) does respond to nanortc's Binding Requests and sees at least one valid pair, then times out waiting for a nominated pair that never arrives.
- Answer-mode tests (T1 DC / T3 audio / T4 audio+video) are all unaffected — CONTROLLED role only replies to inbound requests and does not match its own outstanding transactions.

This bug has existed since the initial ICE implementation; Phase 4's interop suite uses libdatachannel in CONTROLLED mode so it never exercised nanortc as CONTROLLING. RFC 8445 §7.2.5 ("Updating Pair States") requires per-pair transaction tracking for exactly this reason.

**Approach.**

1. Replace the `last_*` scratch triple with a pending-transaction table on `nano_ice_t`:
    ```c
    typedef struct {
        uint8_t txid[STUN_TXID_SIZE];
        uint32_t sent_at_ms;
        uint8_t local_idx;
        uint8_t remote_idx;
        bool in_flight;
    } nano_ice_pending_t;

    nano_ice_pending_t pending[NANORTC_ICE_MAX_PENDING_CHECKS]; /* e.g. 4 */
    ```
    A small fixed table is fine — NanoRTC's candidate pair count is bounded and only a few checks can be outstanding within the ~50 ms RTT of a typical WebRTC session.

2. `ice_generate_check` allocates a free slot, writes `txid`/`sent_at_ms`/indices, and passes the txid to `stun_encode_binding_request`. If the table is full, skip this tick (or evict the oldest).

3. `ice_handle_stun` for `STUN_BINDING_RESPONSE` scans the table for a slot whose `txid` matches `msg.transaction_id`. On match, verifies integrity with `remote_pwd` (existing code), marks that pair as valid, clears the slot, and transitions to CONNECTED.

4. On retransmit, also scan to avoid sending duplicate checks for a pair that already has an in-flight transaction.

5. Time-out stale entries (`now_ms - sent_at_ms > NANORTC_ICE_CHECK_TIMEOUT_MS`, e.g. 5 s) so the table doesn't permanently fill.

6. Add `NANORTC_ICE_MAX_PENDING_CHECKS` to `include/nanortc_config.h` with a `#ifndef` guard and an `#if … #error` validation matching the other ICE limits.

**Secondary concern (fold into same PR).** `NANORTC_EV_ICE_STATE_CHANGE` currently only prints CONNECTED in `examples/browser_interop/main.c` and does not forward intermediate states (`NEW → CHECKING → FAILED`) to the application in a way that surfaces this kind of stall. The ICE module *does* transition to `FAILED` on hitting `NANORTC_ICE_MAX_CHECKS`, and `rtc_process_timers` does emit the state change event. Audit that `rtc->state = NANORTC_STATE_CLOSED` actually propagates out as an application-visible event so future ICE regressions are not silent. If needed, add `NANORTC_EV_DISCONNECTED` to the ICE FAILED branch symmetric to PR-2's SCTP failure event.

**Critical files.**

- `src/nano_ice.h` — add `nano_ice_pending_t`, swap out the scratch triple
- `src/nano_ice.c` — `ice_generate_check` (L243–330), `ice_handle_stun` response branch (L168–230)
- `include/nanortc_config.h` — add `NANORTC_ICE_MAX_PENDING_CHECKS` + sanity `#error`
- `tests/test_ice.c` — add a multi-check-response test that sends 3 requests to different pairs, then responds in reverse order; pre-fix it fails, post-fix it passes
- `tests/test_sizeof.c` — bump `nano_ice_t` upper bound if needed (table is a few dozen bytes)

**Verification.**

- `ctest --test-dir build -R "ice|e2e"` — existing ICE suite must stay green
- New unit test: multi-pair transaction-matching regression (see above)
- `./scripts/run-fuzz.sh 60 fuzz_stun` — make sure the new pending-table lookup doesn't regress fuzz corpus
- **E2E**: re-run `examples/browser_interop` T2 against Chrome 146 headless with `--disable-features=WebRtcHideLocalIpsWithMdns`, asserting that nanortc reaches `[event] Connected` and `DataChannel open` within 10 s. The headless-Chrome + CDP harness built for Phase 7 testing is a good starting point (`/tmp/cdp_drive.py` / `/tmp/cdp_wait.py`).
- Also run T3 / T4 to confirm no regression in CONTROLLED path.

**Risk.** Medium. Touches the ICE state machine, which is the load-bearing module for every connection. Mitigation: the change is localized (one struct field, two functions), has a clear unit test, and the existing Phase 4 interop suite catches any regression on the CONTROLLED path.

**Rollback.** `git revert`.

**Dependency.** None — independent of PR-1..PR-4. Can land in parallel with any of them. Recommend landing early in Phase 8 because it unblocks real browser-as-answerer deployments.

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

- [ ] PR-1, PR-2, PR-3, PR-4, PR-5 all merged on `develop`
- [ ] `ci-check.sh` green on 6 feature combos × 2 crypto backends + ASan + interop
- [ ] Fuzz sweep ≥60s per harness, zero crashes
- [ ] `tests/test_iot_profile` added and passing
- [ ] `tests/test_ice.c` has a multi-pair CONTROLLING transaction-matching test (added by PR-5)
- [ ] `examples/browser_interop` T2 (nanortc --offer vs Chrome headless) passes end-to-end under the Phase 7 CDP harness, demonstrating the PR-5 fix
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
