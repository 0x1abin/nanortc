# Phase 11: Send-side BWE-driven video RTP pacer

**Status:** Implemented + full CI matrix / ASan / libdatachannel interop green; pending adversarial-review sign-off + human browser smoke.
**Estimated effort:** 1 agent session.
**Goal:** Meter outbound video RTP at the BWE-derived rate so a multi-fragment IDR is spread across ~one frame interval instead of bursting onto the wire and overrunning the network bottleneck buffer — the dominant self-inflicted quality killer on an embedded camera's WiFi/cellular uplink.

## Context

This is the egress complement to commit #67 ("atomic frame admission"). #67 fixed
*truncation* — a frame ships whole or returns `WOULD_BLOCK` / `BUFFER_TOO_SMALL`,
never partial. But #67's own comment named the other half and left it open: when a
~60 KB IDR fragments into ~50 RTP packets, all of them landed in `out_queue` at
once and `nanortc_poll_output()` handed them to the app back-to-back, producing a
microsecond burst → bottleneck-buffer overflow → **self-inflicted loss → PLI →
larger IDR → repeat**.

Phase 9 deliberately built BWE (REMB + TWCC loss controller) **"without NanoRTC
itself running a pacer"** — its only consumer was an event telling the app to
change encoder bitrate; the estimate never reached the *egress rate*. That was the
right call at the time (keep the library passive). Phase 11 revisits that non-goal
for the specific embedded-real-time-video goal: egress pacing is the highest-leverage
remaining quality win on the camera uplink, it closes the BWE loop Phase 9 set up,
and it fits the Sans-I/O model cleanly (meter by `now_ms`, surface a deadline via
`nanortc_next_timeout_ms()`). The pacer is **default-on but feature-gated**
(`NANORTC_FEATURE_VIDEO_PACING`) with a proven `=0` fallback to the pre-pacer
immediate-enqueue path, so the passive stance remains one flag away.

## Design

A Sans-I/O leaky token bucket meters **video RTP only**:

- Fragments stage in the existing `pkt_ring` as a `[head, tail)` pace FIFO over the
  same slots the NACK history uses (`tail` tracks `pkt_ring_tail`; every committed
  fragment is pace-enqueued instead of going straight to `out_queue`).
- `nano_rtc_pacer_pump()` runs at the top of `nanortc_poll_output()` using
  `rtc->now_ms`: refill `budget += rate × elapsed / 8000` (capped at
  `NANORTC_PACING_MAX_BURST_BYTES`), then release fragments while
  `budget ≥ len` **or** the oldest has aged past `NANORTC_PACING_MAX_QUEUE_MS`
  (catch-up — the hard latency cap), subject to `out_queue` space.
- Rate = `max(bwe.estimated_bitrate, NANORTC_PACING_MIN_RATE_BPS) ×
  NANORTC_PACING_FACTOR_PCT / 100`.
- `nanortc_next_timeout_ms()` folds in the next-release deadline so the caller's
  loop wakes to pump on schedule (relies on the universal
  `handle_input → drain poll_output → next_timeout → sleep` cadence).

**Invariant preservation (the #67 hot path).** The admission gate in
`nanortc_send_video()` and the `pkt_ring_alloc_slot()` overrun guard now count the
paced-but-unreleased backlog (`pace_depth = pacer.tail − pacer.head`) in addition
to undrained `out_queue` entries, so a new frame's fragments can never wrap over an
unsent pace slot or an undrained output. Whole-frame-or-`WOULD_BLOCK` is preserved;
under pacing the binding capacity is the ring (`NANORTC_VIDEO_PKT_RING_SIZE`), not
`out_queue` (fragments trickle through it over time).

**Bypass list.** NACK retransmits, PLI, RTCP SR, STUN consent, TURN, and audio all
stay on the immediate `nano_rtc_enqueue_transmit` path. Only the two video-fragment
enqueue sites (`rtc_send_video` H.264, `video_send_fragment_cb` H.265) were rerouted.

## Config (`include/nanortc_config.h`, all `#ifndef`-guarded, Kconfig-mirrored)

| Macro | Default | Meaning |
|---|---|---|
| `NANORTC_FEATURE_VIDEO_PACING` | 1 | Enable the pacer (sub-feature of VIDEO; `0` = pre-pacer immediate enqueue). |
| `NANORTC_PACING_FACTOR_PCT` | 150 | Pacing rate as % of BWE estimate (1.5×). |
| `NANORTC_PACING_MAX_BURST_BYTES` | 3000 | Token-bucket burst budget (≥ `MEDIA_BUF_SIZE`). |
| `NANORTC_PACING_MIN_RATE_BPS` | 100000 | Rate floor (anti-stall / div-by-zero). |
| `NANORTC_PACING_MAX_QUEUE_MS` | 40 | Hard cap on latency the pacer may add (catch-up drain). |

## Files

| File | Change |
|---|---|
| `include/nanortc_config.h` | 5 macros + compile-time `#error` validation + Kconfig mirror. |
| `include/nanortc.h` | `nano_pacer_t` + 2 stats counters in `struct nanortc` (VIDEO_PACING-guarded). |
| `src/nano_rtc_internal.h` | `nano_rtc_pacer_enqueue` / `_pump` / `_next_deadline_ms` prototypes. |
| `src/nano_rtc_media.c` | Pacer impl; reroute 2 fragment sites; admission + alloc-guard account for `pace_depth`. |
| `src/nano_rtc.c` | Seed bucket full at init; pump in `poll_output`; deadline in `next_timeout_ms`. |
| `tests/test_video_pacing.c` (new) | 5 deterministic isolation tests (burst, metering, catch-up, deadline, zero-estimate floor). |
| `tests/test_e2e.c` | `test_e2e_video_send_admission` made pacing-aware (pace-FIFO probe + time-advancing flush). |
| `examples/esp32_camera/main/main.c` | `/debug` surfaces `paced` / `pace_catchup` counters. |
| Docs | `ARCHITECTURE.md`, `QUALITY_SCORE.md`, `memory-profiles.md`, this plan. |

## Verification

- `./scripts/ci-check.sh`: **42/42** — 7 combos × openssl/mbedtls (build+test), arch
  constraints, clang-format, ASan (MEDIA), feature-OFF, **libdatachannel interop**
  (the delivery gate — pacing is receiver-transparent, so green proves no wire
  regression).
- `tests/test_video_pacing.c`: 5/5 (metering math, catch-up latency cap, deadline,
  zero-BWE floor).
- Pacing-disabled build (`-DNANORTC_FEATURE_VIDEO_PACING=0`): 28/28 — fallback path
  proven.
- RAM: `nano_pacer_t` adds `4 × PKT_RING_SIZE + 16 B` (~144 B host / ~80 B Kconfig).
- **Adversarial review** (3-dimension fan-out × per-finding verification): 4 findings,
  2 refuted (a WOULD_BLOCK "livelock" and a NACK double-send — both shown
  unreachable), 2 confirmed:
  - *F2 (low)* — backward-clock inconsistency in the token-bucket `aged`/refill paths.
    **Fixed** by making both use plain modular subtraction (codebase convention,
    wrap-correct) + an elapsed clamp; the reviewer's literal `now < x ? 0` patch was
    rejected because it mis-ages packets straddling the ~49-day uint32 wrap.
  - *F1 (medium)* — generic-NACK retransmit pins a non-contiguous pkt_ring slot the
    entry-count admission gate doesn't protect. **Pre-existing & pacing-independent**
    (identical `cap` in the default config; doesn't manifest in the bundled
    run-loops); tracked as **TD-023** with a fix sketch, comment tightened in
    `nano_rtc_media.c`. Out of this PR's scope (would be a NACK refactor).
- **Pending:** human browser smoke (camera → Chrome, confirm no stutter, `/debug`
  `pace_catchup` rare on a healthy link).

## Out of scope (sequenced follow-ups toward "best embedded real-time video")

- Receive-path robustness: video reorder buffer, auto-PLI on loss, receiver NACK gen.
- FEC (ULPFEC/RED) for low-latency loss recovery.
- RTX (RFC 4588), FIR, periodic RR.
