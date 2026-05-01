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
| **PR-2** | Output payload lifetime contract **[LANDED 2026-04-30]** | `include/nanortc.h`, `src/nano_rtc.c`, `tests/test_output_lifetime.c`, `tests/CMakeLists.txt`, `examples/esp32_camera/main/main.c` | Low-medium | Document and regression-test queue pointer lifetime under bursty TURN/video/DC output. |
| **PR-3** | Add `nanortc_next_timeout_ms()` **[COMPLETED 2026-04-29]** | `include/nanortc.h`, `src/nano_rtc.c`, `tests/test_next_timeout.c`, `examples/common/run_loop_{linux,esp}.c` | Low | Let callers block until the next protocol deadline instead of fixed polling. |
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

## PR-2 — Output payload lifetime contract [LANDED 2026-04-30]

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

### Landed work

Single source of truth for the contract is the public-header doxygen at
`include/nanortc.h` (`nanortc_output_t`) — semantics, scratch-buffer reuse,
and the drain-or-copy rule are documented there and not duplicated here.

Landed:

- **Contract docstring** — `include/nanortc.h:406-416` makes the rule explicit:
  pointers in `transmit.data` and `event.*` are valid until the next
  `nanortc_poll_output()` / `nanortc_handle_input()` / `nanortc_destroy()`
  on the same `nanortc_t`; lists every internal scratch (DTLS, STUN, TURN
  wrap, RTP/SRTP, video pkt_ring) that participates in the reuse.
- **Internal enqueue comment** — `src/nano_rtc.c:87-94` re-states the
  lifetime invariant at `rtc_enqueue_transmit()` so future code review
  catches new producers that forget to point into a long-lived buffer.
- **Drop counters** — `nanortc_t.stats_pkt_ring_overrun` (video pkt_ring
  wrap before drain), `stats_wrap_dropped` (lazy TURN wrap exceeds
  `turn_buf`), and `stats_tx_queue_full` (out_queue overflow). The
  tx_queue_full counter was originally TURN-only; PR-2 promoted it to a
  universal field (`include/nanortc.h:697`) so CORE_ONLY/DATA/AUDIO builds
  also surface caller-side drain bugs.
- **Burst regression test** — `tests/test_output_lifetime.c` covers the
  four canonical contract-violation paths through the public
  `nanortc_poll_output()` API: pkt_ring drain-before-wrap, pkt_ring
  overrun-aliases-pre-drain, TURN lazy-wrap rewrites-turn-buf-between-polls,
  TURN oversized-payload-drops-silently. Both `stats_pkt_ring_overrun` and
  `stats_wrap_dropped` are asserted from the test side.
- **Small-ring CI variant** — `tests/CMakeLists.txt` now also builds
  `test_output_lifetime_min` with `NANORTC_OUT_QUEUE_SIZE=4` +
  `NANORTC_VIDEO_PKT_RING_SIZE=4` overrides (per the §Verification third
  bullet). At default sizes the burst the test generates fits without wrap;
  the small-ring variant guarantees the alias window is small enough to
  trip in CI.

### Hardware verification (ESP32-P4 nano, 2026-04-30)

Validated end-to-end on `examples/esp32_camera` running on an ESP32-P4
nano board against Chrome 1568×778 viewport, 1080p H.264 + Opus stream.

`webrtc_task` and `/debug` HTTP endpoint were extended with periodic
lifetime audit dumps so the bench operator can observe the counters
without a working serial monitor.

| Run | Config | Duration | Result |
|---|---|---|---|
| Baseline | default 32/32 rings | 5 min | `pkt_overrun=0`, `tx_full=0`, `wrap_drop=0`; browser `framesDecoded` grew monotonically 999 → 5316 (~14.5 fps); `webkitDroppedFrameCount` jumped twice in 30s windows but those are HTML5 playback drops (macOS power throttling) — currentTime advanced cleanly, no decoder corruption |
| Small-ring stress | `OUT_QUEUE_SIZE=8 + VIDEO_PKT_RING_SIZE=8` via `sdkconfig.defaults.esp32p4` | 5 min | `pkt_overrun=114` (~0.24/s), `tx_full=311` (~1/s), `wrap_drop=0`, `direct=44853` total enqueues (drop rate 0.69%); browser locked in **clean black frame** (no mosaic / green blocks / pixel-aliased garbage), `framesDecoded` froze at 33 (initial keyframe only) and stayed there — every subsequent 1080p keyframe overflowed pkt_ring=8 (each keyframe is ~80–150 FU-A fragments) and the contract dropped them in full rather than ship aliased fragments. Device `steps`/`alive` continued growing linearly throughout — no hangs, no crashes. |

The small-ring failure mode is the success signal: the lifetime contract
prefers losing whole frames over surfacing aliased payloads.

### Resource sweep follow-up (2026-05-01)

A subsequent attempt to shrink the example's ring sizes (16/16, then
32/16) initially looked clean over a 5-min static-scene bench but
broke as soon as the camera saw real motion: 1080p P-frames in motion
hit 12–15 KB → 11–13 FU-A fragments per `nanortc_send_video()` call,
which under FreeRTOS scheduling jitter and concurrent audio/RTCP/ICE
traffic kept tripping `tx queue full, dropping output` and momentary
playback freezes. **32/32 is the practical floor** for this profile;
`examples/esp32_camera/sdkconfig.defaults` keeps `OUT_QUEUE_SIZE=32`
(pkt_ring inherits) and bumps `EXAMPLE_H264_BITRATE_KBPS` from 1024
to 2048 (Kconfig default) so the encoder isn't starving detail in
moving scenes — the rings are sized to absorb the resulting bursts.
Targets that produce smaller access units (lower bitrate, shorter
GOP, software encoder, IoT-only data path) can still try smaller
rings via the same `/debug` audit counters, but the sweep needs a
moving scene and `> 5 min` window to be meaningful.

The temporary `CONFIG_NANORTC_*_SIZE=8` lines in
`sdkconfig.defaults.esp32p4` were reverted after the stress run.

## PR-3 — `nanortc_next_timeout_ms()` [LANDED]

Public deadline-aggregator API so embedded loops can block in
`select()` / `poll()` / `epoll_wait()` / RTOS waits up to the next protocol
tick instead of polling on a fixed cadence. Previously the only signal
was `NANORTC_OUTPUT_TIMEOUT`, which forces a `poll_output()` round-trip
before every blocking wait.

Single source of truth for the contract is the public-header doxygen at
`include/nanortc.h` (`nanortc_next_timeout_ms`) — semantics, aggregated
deadlines, and idle cap are documented there and not duplicated here.

Landed:

- API + implementation: `include/nanortc.h` / `src/nano_rtc.c`.
- Unit coverage: `tests/test_next_timeout.c` exercises idle cap, DTLS
  handshake clamp to `NANORTC_MIN_POLL_INTERVAL_MS`, ICE check pacing,
  ICE consent freshness, STUN srflx retry, TURN refresh, and the
  multi-source `min`. SCTP RTO and RTCP cadence are plumbing-identical
  and covered structurally rather than via a heavy fixture.
- Real callsite: `examples/common/run_loop_linux.c:nano_run_loop_step`
  and the mirrored `examples/common/run_loop_esp.c:nano_run_loop_step`
  call the aggregator before `select()`, shortening the user's
  `max_poll_ms` ceiling whenever a protocol deadline is closer.

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
- [x] Output pointer lifetime is documented and covered by regression tests (PR-2, 2026-04-30) — `include/nanortc.h:406-416` + `tests/test_output_lifetime.c` (default + `_min` variant) + ESP32-P4 nano hardware bench.
- [x] `nanortc_next_timeout_ms()` implemented (PR-3, 2026-04-29) — public API + `tests/test_next_timeout.c` + run_loop integration on Linux and ESP-IDF.
- [ ] Any `nano_rtc.c` split keeps all feature combinations compiling.
- [ ] No Sans-I/O, no-malloc, feature-guard, or safe-C constraint is weakened.

## Non-goals

- Rewriting protocol modules.
- Introducing a pacer or active congestion-control enforcement inside NanoRTC.
- Breaking the existing public API during this phase.
- Depending on third-party protocol implementations for wire behavior.
