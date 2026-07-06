# Adaptive Video — Spec & Send-Strategy Control

**Status:** Requirement + implementation status + roadmap. Phase A is implemented:
the SDK has a pure-compute adaptive controller, a capability-ladder API, and a
recommendation event; the Linux UVC example applies the recommendation to bitrate
and, on the FFmpeg backend, resolution/fps.

This document defines the adaptive-video capability NanoRTC aims to provide, records how
far the current code meets it, and lays out the remaining work. It is the design companion
to core belief #10 ([core-beliefs.md](../design-docs/core-beliefs.md)) and to the feature
matrix in [deployment-profiles.md](deployment-profiles.md).

## 1. The requirement

Under the real, time-varying bandwidth of **both** endpoints, the stack must continuously
converge the video **spec** — resolution, framerate, bitrate — and its **send strategy**
toward what the bottleneck path can carry, ranking **latency and playback smoothness above
picture detail**, and it must do so as a **reusable SDK capability** rather than per-app glue.

Concretely, the target behavior:

- Converge to the **bottleneck** of the two ends (sender uplink vs receiver downlink).
- When the path narrows: keep the stream real-time and smooth by **stepping down
  resolution/framerate**, not only by squeezing the encoder's QP at a fixed geometry.
- When the path recovers: **step back up**, promptly but without oscillation.
- Keep **end-to-end latency bounded** (a frozen-but-sharp picture is a failure for
  real-time).
- Expose this as an SDK component an application configures with its encoder's capability
  ladder + a policy, not a hand-rolled control loop.

Sans I/O (belief #1) constrains *where* this can live: the SDK must not own the encoder or
camera. The capability therefore splits into **decide** (pure compute, in the SDK) and
**apply** (I/O, in the caller).

## 2. Current state — what exists today

### 2.1 Transport substrate (solid)

| Capability | Where | Notes |
|---|---|---|
| Bottleneck bandwidth estimate | `src/nano_bwe.c` (`bwe_on_rtcp_feedback` REMB, `bwe_on_twcc_loss`) | Converges to the min path; EMA-smoothed (`NANORTC_BWE_EMA_ALPHA`). |
| Estimate exposed to app | `include/nanortc.h`: `NANORTC_EV_BITRATE_ESTIMATE` (with `direction`/`source`), `nanortc_get_estimated_bitrate`, `nanortc_set_bitrate_bounds` / `_initial_bitrate` / `_bwe_event_threshold` | App reads the estimate; tunes bounds. |
| Send pacing (anti-burst) | `src/nano_rtc_media.c` `nano_rtc_pacer_pump`; `NANORTC_PACING_*` in `include/nanortc_config.h` | Token bucket at `PACING_FACTOR_PCT × BWE`, latency cap `MAX_QUEUE_MS`. |
| Loss recovery | NACK-rx, FEC (adaptive group size K by loss), reorder, auto-PLI; flags `NANORTC_FEATURE_VIDEO_{NACK_RX,FEC,REORDER,AUTO_PLI}` | Per [deployment-profiles.md](deployment-profiles.md); FEC K already tracks smoothed TWCC loss. |
| Keyframe-on-demand | `nanortc_request_keyframe` (RTCP PLI) | App-driven; auto-PLI on receive-side gaps. |

The transport layer is also **geometry-agnostic**, so it does not block spec changes:
`nanortc_send_video` does not inspect width/height; H.264/H.265 packetizers fragment NALs by
MTU only; in-band SPS/PPS (and H.265 VPS) ride as ordinary NALs; the reorder buffer and the
`rate_window` fps/bitrate meter (`src/nano_media.c`) hold no geometry state; RTP timestamps use
the fixed 90 kHz clock, independent of fps.

### 2.2 The adaptation loop (Phase A implemented)

The SDK now owns the reusable **decision** step. Applications install a caller-owned
capability ladder with `nanortc_set_capability_ladder()`, and the SDK's
`nano_rate_control` module maps the live BWE estimate + smoothed loss to a recommended
ladder rung. The recommendation is surfaced as `NANORTC_EV_SPEC_RECOMMENDATION`; the
application still owns the **apply** step because Sans I/O forbids the library from
touching encoders or cameras.

```
BWE/TWCC feedback ──▶ src/nano_rate_control.c
       └─NANORTC_EV_SPEC_RECOMMENDATION──▶ examples/linux_uvc_camera/main.c
            ├─ aggregate min rung across viewers (single shared encoder)
            ├─ capture_set_bitrate()
            └─ capture_set_layout() on the FFmpeg backend
```

The FFmpeg/NVENC backend reopens the encoder at the recommended geometry, frame-skips
to the recommended fps, refreshes in-band H.265 parameter sets, and forces an IDR. The
GStreamer backend still returns `-1` from `capture_set_layout()` because its caps are
baked into the launch pipeline; it remains bitrate-adaptive only.

## 3. Gap analysis — does it meet the requirement?

**Verdict: Phase A closes the biggest SDK packaging gap.** The transport substrate is
strong, and the spec-adaptation brain now exists as a default SDK controller. Remaining
work is mostly signal quality, spec-change packaging parity, and multi-direction policy.

| # | Gap | Impact on the requirement | Evidence |
|---|---|---|---|
| G1 | **Resolved for the SDK + FFmpeg example; partial across all apps/backends.** | The library can now recommend resolution/fps/bitrate rungs; applications still must apply them. | `nano_rate_control`, `nanortc_set_capability_ladder`, `NANORTC_EV_SPEC_RECOMMENDATION`; FFmpeg `capture_set_layout()` applies geometry, GStreamer remains bitrate-only. |
| G2 | **Default policy implemented; no custom policy API yet.** | Realtime/smoothness-first behavior is covered, but apps cannot tune preference weights beyond compile-time constants. | Safety/headroom/hold/loss thresholds live in `NANORTC_RATE_CONTROL_*`. |
| G3 | **SDK decision implemented; application apply loop remains by design.** | Apps no longer re-roll the controller, but each encoder backend still owns the I/O-specific application step. | Controller lives in `src/`; Linux UVC example aggregates recommendations and applies the shared-encoder min rung. |
| G4 | **BWE is loss-only and asymmetric.** | Drops fast, recovers slowly, and reacts only *after* loss (queues already overflowed) → hurts both "step back up on recovery" and "bounded latency". | Tracked as **issue #71 / TD-027**; per-packet TWCC delay is parsed then discarded (`twcc_parse_feedback(..., NULL, NULL)` in `src/nano_rtc_media.c`). |
| G5 | **Spec-change flow is not packaged.** | An app that *did* step resolution must hand-roll force-IDR + in-band re-send of parameter sets; the SDP `sprop-parameter-sets` is offer/answer-only and goes stale; there is no H.264 parameter-set API (only `nanortc_video_set_h265_parameter_sets`). | `src/nano_sdp.c` sets sprop at negotiation only; no H.264 equivalent. |
| G6 | **Two-way (Profile C) has no per-direction controller.** | A symmetric intercom needs an independent controller per direction. | Profile C documented in [deployment-profiles.md](deployment-profiles.md); no controller. |

What the requirement **does** already get: bottleneck convergence, SDK-level default rung
selection, anti-burst pacing, loss resilience, and a transport layer that does not block
resolution/fps changes.

## 4. Architecture recommendation (Sans-I/O compatible)

Keep belief #1 intact by splitting **decide** (SDK, pure compute) from **apply** (caller, I/O).

### 4.1 An Adaptive Media Controller as an SDK capability

The library implementation is `nano_rate_control`, the SDK-level successor to the
example's old bitrate-only decision loop:

- **Current inputs**: current bottleneck estimate, smoothed loss, an **application capability
  ladder** (the ordered set of `{w, h, fps, bitrate}` rungs the encoder supports), and
  compile-time policy constants. Phase B adds a delay/probing signal (see §4.3).
- **Current output**: a recommended rung `{resolution, fps, bitrate}`, with **hysteresis** —
  asymmetric up/down thresholds and a minimum hold time — so it does not flap between rungs.
  Send-strategy hints such as pacer/FEC tuning remain future work.
- **Purity**: a function of its inputs only; it touches no encoder, socket, or clock. It fits
  `src/` and is exercised by host unit tests like the existing BWE/pacer tests. The caller
  receives the recommendation (e.g. via a new event or a poll call) and applies it to its
  encoder — exactly the decide/apply split of belief #10.

### 4.2 Default policy: realtime + smoothness first

- Prefer **lower framerate/resolution** over letting per-pixel bitrate fall below a quality
  floor or letting the encoder drop frames.
- Keep added latency bounded (small pacer `MAX_QUEUE_MS`); never trade latency for detail.
- Step **down** quickly on congestion; step **up** only after stable headroom is confirmed
  (needs §4.3).

### 4.3 Fix the congestion signal (prerequisite for clean up-adaptation)

Resolve **#71 / TD-027**: feed the discarded per-packet TWCC arrival-delay data into a
delay-based (GCC-style) estimator and add light active probing. This makes adaptation
**proactive** (reacts to queue buildup before loss → lower latency) and **symmetric** (can
discover recovered headroom instead of parking at the floor), which the up-step in §4.2
depends on.

### 4.4 Package the spec-change protocol

On a rung change, the controller's recommendation should drive: force an IDR
(`nanortc_request_keyframe` / encoder forced-IDR) and re-emit parameter sets in-band. Add an
H.264 parameter-set API to match the H.265 one, and document the receiver contract (consume
new VPS/SPS/PPS, wait for IDR before decoding the new geometry).

**Receiver contract (implemented in Phase 14 / phase14 PR-3+PR-4).** On a geometry change the
sender forces an IDR and re-emits the parameter sets **in-band** as a multi-NAL access unit
(`[VPS][SPS][PPS][IDR]` for H.265, `[SPS][PPS][IDR]` for H.264) — there is no mid-stream SDP
update (the SDP `sprop-*` fmtp is offer/answer-only and goes stale). The receiver consumes the
new parameter sets and waits for the IDR before decoding the new geometry. The nanortc transport
is geometry-agnostic (packetizers fragment by MTU only, no width/height state), so the switch
needs no renegotiation. Verified by `tests/test_e2e.c::test_e2e_h265_midstream_param_refresh`
(mid-session param-set re-set + IDR consumed) and
`tests/interop/test_interop_video.c::test_interop_video_h265_midstream_refresh` (the multi-NAL
spec-switch AU reaches a libdatachannel receiver). The H.264 out-of-band parameter-set API to
match `nanortc_video_set_h265_parameter_sets` remains Phase C; H.264 currently relies on the
in-band path, which is sufficient for the mid-stream switch.

## 5. Optimization roadmap

Phased so each step is independently shippable and testable; ordered by value.

- **Phase A — Adaptive Media Controller (biggest win).** Pure-compute ladder + policy +
  hysteresis as an SDK module; wire the example encoder to drive resolution **and** fps **and**
  bitrate. Verify on the H2O 4K↔1080p↔720p path.
- **Phase B — Proactive/symmetric BWE.** Delay-based estimate + probing; resolves #71/TD-027.
  Unlocks clean up-adaptation and lower latency.
- **Phase C — Spec-change protocol hardening.** H.264 parameter-set API parity, a
  force-IDR-on-switch helper, and the receiver contract in docs.
- **Phase D — Two-way controllers.** Independent per-direction control for Profile C.

## 6. Constraints any implementation must honor

- **Sans I/O** (belief #1): the controller is pure compute; the caller owns the encoder.
- **Caller owns resources** (belief #5): the capability ladder and any working buffers are
  caller-provided; no allocation in `src/`.
- **Compile-time features** (belief #4): gate the controller behind a `NANORTC_FEATURE_*` flag
  so non-adaptive embedded builds pay nothing.
- **Mechanical enforcement** (belief #7): ship host unit tests (ladder selection, hysteresis,
  policy ordering) and an interop check alongside the code.
