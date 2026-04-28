# Phase 10: Design Convergence

**Status:** Active — planning document created after design review.
**Estimated effort:** 2–3 agent sessions, split into independent PRs.
**Goal:** Convert the current design review findings into small, reviewable changes that keep NanoRTC's embedded/Sans-I/O constraints intact while reducing future integration risk.

## Context

NanoRTC's core design is sound: pure C, Sans I/O, no heap allocation, caller-owned networking/time, compile-time feature trimming, RFC-derived protocol logic, and fixed-size state in `nanortc_t`. The implementation has also moved quickly through TURN, IPv6, H.265, TWCC/BWE, memory trimming, and interop hardening.

The main risk is now convergence rather than missing protocol blocks:

- The design documents, architecture overview, quality score, and active plans can drift because the implementation evolves faster than the original design draft.
- `src/nano_rtc.c` is the load-bearing orchestration layer for ICE, TURN, DTLS, SCTP, media, BWE, timers, and output queue ownership.
- `nanortc_output_t.transmit.data` is pointer-based, so payload lifetime rules must stay crisp for TURN lazy wrapping, video packet rings, SCTP output, and future zero-copy work.

This phase is intentionally conservative: prefer additive APIs, documentation invariants, and internal refactors over protocol behavior changes.

## Sub-PRs

| PR | Topic | Primary files | Risk | Target |
|---|---|---|---|---|
| **PR-1** | Documentation convergence guardrails | `docs/design-docs/nanortc-design-draft.md`, `ARCHITECTURE.md`, `docs/QUALITY_SCORE.md`, `docs/PLANS.md` | Low | Make one current design source of truth and remove stale feature/resource statements. |
| **PR-2** | Output payload lifetime contract | `include/nanortc.h`, `ARCHITECTURE.md`, `tests/test_media.c`, `tests/test_turn.c`, `tests/test_e2e.c` | Low-medium | Document and regression-test queue pointer lifetime under bursty TURN/video/DC output. |
| **PR-3** | Add `nanortc_next_timeout_ms()` | `include/nanortc.h`, `src/nano_rtc.c`, timer-focused tests, examples | Low | Let callers block until the next protocol deadline instead of fixed polling. |
| **PR-4** | Split RTC orchestration internals | `src/nano_rtc.c`, new `src/nano_rtc_*.c/.h`, `CMakeLists.txt`, ESP-IDF component sources | Medium | Reduce single-file complexity without changing public API or module dependencies. |

## PR-1 — Documentation convergence guardrails

### Problem

The design draft historically described an earlier state of the project: fewer feature combinations, old resource estimates, older phase status, and references to third-party implementations as design guides. Newer docs already carry the current architecture and quality status, but readers can still land on stale text.

### Approach

1. Update the feature flag table to include TURN, IPv6, H.265, TWCC/BWE-related video modules, and the current seven canonical feature combinations.
2. Replace old resource estimates with a pointer to `docs/engineering/memory-profiles.md` plus a compact current matrix.
3. Replace week-based historical implementation schedules with agent-session phase status and links to `docs/PLANS.md`.
4. Clarify that RFCs are authoritative for protocol behavior; third-party projects are engineering context or interop fixtures, not sources for wire formats/state machines.
5. Add a short "design freshness" checklist to `docs/PLANS.md` or the development workflow so future protocol work updates the same core docs.

### Verification

- Link/reference scan for stale "6 combinations", old RAM estimates, or libjuice-as-implementation wording.
- `./scripts/ci-check.sh --fast` if code-adjacent files are touched; otherwise at least inspect docs-only diff and run a markdown link/path sanity check where available.

## PR-2 — Output payload lifetime contract

### Problem

NanoRTC's output queue stores pointers instead of copying every payload. That is important for memory, but it makes lifetime subtle:

- TURN relay output is wrapped lazily in `nanortc_poll_output()` using `turn_buf`.
- Video transmit output points into `pkt_ring[]` slots.
- Media packetization can enqueue many fragments before the caller drains them.
- Future zero-copy changes will make ownership even more important.

### Approach

1. Document the exact lifetime for every `NANORTC_OUTPUT_TRANSMIT` pointer:
   - valid until the next successful `nanortc_poll_output()` call, next mutating API call, or `nanortc_destroy()`; choose and enforce one rule in code/tests.
   - callers must copy or send synchronously before polling again if they need retention.
2. Add internal comments near `rtc_enqueue_transmit()` and TURN/video enqueue paths.
3. Add burst tests that intentionally queue multiple TURN-wrapped and video-fragment outputs, poll them one by one, and verify payloads are not aliased unexpectedly.
4. Add a debug/stat counter if an output producer must drop due to ownership constraints.

### Verification

- Existing media/TURN/e2e tests.
- New aliasing regression tests under small `NANORTC_OUT_QUEUE_SIZE` and small `NANORTC_VIDEO_PKT_RING_SIZE` overrides.
- Parametrize the burst tests to `NANORTC_OUT_QUEUE_SIZE=4` + `NANORTC_VIDEO_PKT_RING_SIZE=4` so the aliasing window is small enough to actually trigger in CI; at default sizes the window is wide enough that regressions can slip through unnoticed.

## PR-3 — Add `nanortc_next_timeout_ms()`

### Problem

Applications currently learn the next protocol wakeup through `NANORTC_OUTPUT_TIMEOUT`, but many embedded loops want a direct query before blocking in `select()`, `poll()`, `epoll_wait()`, or an RTOS event wait. Fixed periodic ticking wastes CPU and power.

### Proposed API

```c
int nanortc_next_timeout_ms(const nanortc_t *rtc, uint32_t now_ms, uint32_t *out_ms);
```

Semantics:

- `out_ms=0` means call `nanortc_handle_input()` immediately with a timer tick.
- A positive value is the maximum delay before the next required tick.
- Return `NANORTC_ERR_INVALID_PARAM` on NULL arguments.
- Return `NANORTC_ERR_STATE` for destroyed/uninitialized state only if existing API has a comparable state guard; otherwise keep it total and conservative.

### Internal deadlines to consider

- ICE connectivity checks and pending transaction expiry.
- ICE consent freshness / timeout.
- STUN srflx retry.
- TURN Allocate/Refresh/CreatePermission/ChannelBind timers.
- DTLS retransmit / close_notify progress if exposed by the crypto provider.
- SCTP RTO / heartbeat.
- RTCP periodic send.
- Minimum poll cadence (`NANORTC_MIN_POLL_INTERVAL_MS`) as a safety cap when no finer-grained deadline exists.

### Verification

- Unit tests covering core-only, datachannel, TURN-enabled, and media builds.
- Example update showing event-loop use.
- Full feature matrix because timer code crosses feature guards.
- Add a wakeup-aware integration in `examples/desktop/*` (`select()` / `poll()` / `epoll_wait()` driven by `nanortc_next_timeout_ms()`) so the API has a real callsite the moment it lands, not just a header declaration.

## PR-4 — Split RTC orchestration internals

### Problem

`src/nano_rtc.c` is the right architectural owner for top-level orchestration, but it is large enough that independent concerns are harder to review. This increases risk when adding timers, media feedback, TURN wrapping, or state propagation.

### Approach

Split by internal concern while preserving the public API and module dependency graph:

- `nano_rtc_core.c` — public API entry points and common queue helpers.
- `nano_rtc_timers.c` — deadline/timer processing.
- `nano_rtc_ice.c` — ICE/STUN/TURN receive and candidate helpers.
- `nano_rtc_media.c` — RTP/SRTP/RTCP/BWE media paths.
- Keep `nano_rtc.c` as the public compilation unit only if build churn is too high; otherwise move code incrementally.

Rules:

- No new public headers.
- No callbacks between protocol modules.
- No malloc, no platform headers.
- Keep static helpers private to the new file that owns the concern.

### Verification

- `./scripts/ci-check.sh --fast` for each split step.
- Full feature matrix once all file moves are complete.
- Pay special attention to ESP-IDF source lists.

## Acceptance Criteria

- [ ] Design draft, architecture overview, plan index, and quality score agree on current feature matrix and module status.
- [ ] Output pointer lifetime is documented and covered by regression tests.
- [ ] `nanortc_next_timeout_ms()` is implemented or explicitly deferred with a testable reason.
- [ ] Any `nano_rtc.c` split keeps all feature combinations compiling.
- [ ] No Sans-I/O, no-malloc, feature-guard, or safe-C constraint is weakened.

## Non-goals

- Rewriting protocol modules.
- Introducing a pacer or active congestion-control enforcement inside NanoRTC.
- Breaking the existing public API during this phase.
- Depending on third-party protocol implementations for wire behavior.
