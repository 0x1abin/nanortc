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
| **PR-3** | Video `pkt_ring` decoupled from `OUT_QUEUE_SIZE` **[COMPLETED 2026-04-23]** | `include/nanortc.h`, `include/nanortc_config.h`, `Kconfig`, `src/nano_rtc.c`, `tests/test_media.c`, `docs/engineering/memory-profiles.md` | Low | −19 KB at `PKT_RING=16` (host VIDEO profile); −9.6 KB at `PKT_RING=8` (ESP-IDF Kconfig defaults) |
| **PR-4** | H.264 FU-A zero-copy via fragment iterator **[COMPLETED 2026-04-25]** | `src/nano_h264.h`, `src/nano_h264.c`, `src/nano_rtp.c`, `src/nano_rtc.c`, `tests/test_h264.c`, `tests/test_rtp.c` | Medium (internal API change) | −1200 B stack + 1 memcpy/fragment (~50–100 KB/720p IDR) |
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

## PR-2 — SCTP connection-failure event propagation **[COMPLETED 2026-04-25]**

**Problem.** When `nsctp_handle_timeout` exceeds `NANORTC_SCTP_MAX_RETRANSMITS`, it transitions the SCTP state to `CLOSED` but does not notify the upper layer. Applications only learn about the loss on the next `nanortc_datachannel_send()` failure. This is a silent stall from the app's point of view.

**Shipped.**

1. New `closed_due_to_failure` flag on `nano_sctp_t` (`src/nano_sctp.h`), set exactly once in `nsctp_handle_timeout()` when retransmit exhaustion trips the CLOSED transition (`src/nano_sctp.c`). Init-time CLOSED and peer-ABORT-induced CLOSED deliberately do **not** set the flag — they have separate signaling channels (rationale recorded inline in the header comment).
2. RTC poll layer reads and clears the flag in `rtc_process_timers` (`src/nano_rtc.c`), then emits `NANORTC_EV_DISCONNECTED` and transitions `nanortc_t.state` to `NANORTC_STATE_CLOSED`. Mirrors the existing ICE-failure / consent-expiry paths. Critical detail: emit block is placed **outside** the `if (sctp.state == ESTABLISHED)` guard — once timeout flips state to CLOSED, that guard goes false and the signal would otherwise be lost.
3. Two regression tests in `tests/test_sctp.c`:
   - `test_sctp_timeout_sets_closed_flag` (under `#if NANORTC_FEATURE_DC_RELIABLE`) — drives the full RTO ramp (1000 → 2000 → 4000 → 8000 → 10000 capped) through `MAX_RETRANSMITS` cycles of `nsctp_handle_timeout()` + `nsctp_poll_output()`, asserts CLOSED + flag on the threshold-trip iteration.
   - `test_sctp_abort_does_not_set_failure_flag` — scope guard ensuring peer ABORT does not regress into the failure flag.

**Verification.** Full `ctest` (test_sctp 56/56 with DC_RELIABLE=ON, 55/55 with DC_RELIABLE=OFF), `scripts/ci-check.sh --fast` (15/15 incl. ASan + ICE_SRFLX=OFF combo + format check), and full feature-flag matrix via DC_RELIABLE toggle.

**Risk realised.** Low. Existing tests untouched; new flag is one-shot, so no observable behavior change unless a real timeout actually happens. The single non-obvious point — emit block outside the ESTABLISHED guard — is documented inline.

**Rollback.** Revert the four changed files: `src/nano_sctp.h`, `src/nano_sctp.c`, `src/nano_rtc.c`, `tests/test_sctp.c`.

---

## PR-3 — Video `pkt_ring` decoupled from `OUT_QUEUE_SIZE` **[COMPLETED 2026-04-23]**

**Problem.** `nanortc_t.pkt_ring[NANORTC_OUT_QUEUE_SIZE][NANORTC_MEDIA_BUF_SIZE]` is 32 × 1232 = 38.5 KB and drives the vast majority of the full-media memory footprint. The ring serves two unrelated roles — generic output queueing and NACK retransmit window — and both are currently sized by the same macro, so tuning either direction is coarse.

**Shipped (PR #54, commits `2c76486` … `b08b9cf`).**

1. Introduced `NANORTC_VIDEO_PKT_RING_SIZE` in `include/nanortc_config.h` with `#ifndef` guard, Kconfig hook, and compile-time `#error` checks for power-of-2 and `>= 4`. Default = `NANORTC_OUT_QUEUE_SIZE` (byte-identical to pre-PR behavior).
2. Retargeted `pkt_ring[]` and `pkt_ring_meta[]` to the new macro under `#if NANORTC_FEATURE_VIDEO` and added an independent `pkt_ring_tail` cursor (decoupled from `out_tail`).
3. Updated `video_send_fragment_cb` (`src/nano_rtc.c`) to derive its slot from `pkt_ring_tail`, and the NACK retransmit scan in `rtc_process_receive` to iterate `NANORTC_VIDEO_PKT_RING_SIZE` instead of `NANORTC_OUT_QUEUE_SIZE`.
4. **Sizing rule documented as a hard constraint, not a drain hint** (review feedback, commit `b08b9cf`):

   ```
   NANORTC_VIDEO_PKT_RING_SIZE >= ceil(max_frame_bytes / NANORTC_VIDEO_MTU) + 1
   ```

   `out_queue[].transmit.data` stores a *pointer* into `pkt_ring[]`, and `nanortc_send_video()` emits every FU-A fragment of one access unit before returning to the caller — there is no opportunity to drain mid-frame. Wrapping `pkt_ring_tail` while earlier-fragment pointers are still pending in `out_queue` silently corrupts those buffers (receiver sees newer fragment bytes carrying older RTP seq). The earlier draft text recommending "`PKT_RING=16` ≈ 500 ms NACK window at 30 fps" was wrong for multi-NAL frames at 720p+ and was removed; `docs/engineering/memory-profiles.md` now carries the worked-numbers table (480p ≈ 16, 720p ≈ 32, 1080p ≈ 64).
5. **Runtime overrun guard + observability counter** (review feedback, commit `c1cb699`; helper-extracted in PR #57 `pkt_ring_alloc_slot`). The slot allocator checks `(out_tail - out_head) >= PKT_RING_SIZE` before handing back the buffer, atomically bumps `nanortc_t.stats_pkt_ring_overrun`, and emits a single `NANORTC_LOGW` so under-sizing surfaces in integration smoke tests rather than as glitched IDRs on the wire. Counter is exposed on the struct for app glue to read. After PR-4 (#56) introduced the H.264 zero-copy path, both the H.264 (`rtc_send_video`) and H.265 (`video_send_fragment_cb`) writers now share the same allocator + commit helper.
6. **Tests** (`tests/test_media.c`): five new tests under `#if NANORTC_FEATURE_VIDEO` — `in_window_lookup`, `out_of_window_miss`, `wraparound_independent_of_out_tail` (PR baseline), plus `overrun_counter_fires_when_undersized` and `aliasing_corrupts_pending_pointers_when_undersized` (review). Verified under default build (`PKT_RING=OUT_QUEUE=32`) and IoT override (`-DNANORTC_VIDEO_PKT_RING_SIZE=8 -DNANORTC_OUT_QUEUE_SIZE=32`).

**Verification.** All 7 canonical feature combos × {mbedtls, openssl} green, including ASan and libdatachannel interop. `scripts/ci-check.sh --fast` 15/15 passes.

**Review feedback closed.**

- Copilot (`#discussion_r3128279481`): "≈19 KB" claim was wrong for ESP-IDF Kconfig defaults. Fixed in `91d175b` by rewriting the help text around the formula `(OUT_QUEUE_SIZE − PKT_RING_SIZE) × MEDIA_BUF_SIZE` with worked examples for both host and Kconfig.
- Internal review (this branch): docs framed the safety property as "drain each tick" but `nanortc_send_video()` emits every fragment before returning, so the real invariant is the per-frame fragment bound. Addressed by `c1cb699` (runtime guard) + `984e46e` (regression test) + `b08b9cf` (docs rewrite + worked-numbers table).

**Rollback.** Revert PR #54 commits or set `NANORTC_VIDEO_PKT_RING_SIZE` to `NANORTC_OUT_QUEUE_SIZE` (the default) for byte-identical behavior.

---

## PR-4 — H.264 FU-A zero-copy via fragment iterator **[COMPLETED 2026-04-25]**

**Problem.** `h264_packetize()` allocated `uint8_t pkt[NANORTC_VIDEO_MTU]` (1200 B) on the stack for every fragment and then `memcpy`d the FU-A payload into it. ESP32 video tasks run with ~4 KB stack; 1200 B was a 30% footprint and a correctness hazard. The `memcpy` was also pure overhead — the data was about to be `memcpy`d again into `pkt_ring[slot]` by the caller.

**Landed approach** (Option C — iterator instead of monolithic packetizer; PR #56 / commit `fbb5b4f`).

1. Added `h264_fragment_iter_*` API in `src/nano_h264.{h,c}`. The iterator lets the caller pass a different scratch buffer per fragment, which is the precondition for writing each FU-A payload directly into the final `pkt_ring[]` slot.
2. `h264_packetize()` was kept as a thin compat wrapper (tests still use it) with a new `(scratch, scratch_len)` parameter — internal API break, but the function is `@internal` and not in `include/nanortc.h`.
3. `rtp_pack()` (`src/nano_rtp.c:75`) now guards its payload memcpy with `payload != buf + off`. Strictly additive — every existing caller is byte-identical. The H.264 send path passes `pkt_buf + off` as the iterator scratch, so the guard fires on every fragment and the second copy is skipped.
4. `src/nano_rtc.c:rtc_send_video` was rewritten to drive the iterator directly. `pkt_ring` slot selection + the PR-3 overrun guard were carried over (and as of PR #57 are extracted into `pkt_ring_alloc_slot` / `pkt_ring_commit_slot` helpers shared with the H.265 callback).
5. `video_send_ctx_t` / `video_send_fragment_cb` stay alive under `NANORTC_FEATURE_H265` — `h265_packetize_au` is still callback-driven; a parallel zero-copy refactor for it is a separate PR.

**Tests.** `tests/test_h264.c` 13 existing sites updated to pass scratch; three new tests (iterator writes in place at the caller's buffer, rejects under-sized scratch, single-NAL fast path leaves scratch untouched). `tests/test_rtp.c` two new tests asserting `rtp_pack` with `payload == buf + off` produces the same bytes as the copy path, both with and without the TWCC extension.

**Measured savings** (from `otool -tV` on the post-merge binary): no function in the H.264 send chain reserves a 1200 B frame any more — `rtc_send_video` 240 B, `h264_packetize` 192 B, `h264_fragment_iter_next` 96 B. One `memcpy` per FU-A fragment removed, ~50–100 KB per 720p IDR.

**Verification.** `scripts/ci-check.sh --fast` 15/15 (clang-format, sans-I/O include audit, no-malloc audit, unbounded-string audit, struct-array-size audit, DATA + MEDIA + ASan + ICE_SRFLX-off feature combos), full host suite 21/21, libdatachannel interop green.

**Rollback.** Revert PR #56. The compat wrapper for `h264_packetize` accepts the old call shape only with the new (scratch, scratch_len) parameters threaded through, so a partial revert would break the in-tree H.264 tests.

---

## PR-5 — ICE CONTROLLING per-pair pending transaction table **[COMPLETED 2026-04-13]**

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
Change `nanortc_input_t.data` from `const uint8_t *` to `uint8_t *` so inbound SRTP can be unprotected in place without the scratch copy. Breaking API change — requires major version bump. Defer until profiling proves the RX memcpy is a bottleneck.

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
