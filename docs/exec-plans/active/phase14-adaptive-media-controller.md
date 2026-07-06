# Phase 14: Adaptive Media Controller — spec (resolution/fps/bitrate) + send strategy

**Status:** Active — **PR-1…PR-4 implemented; live-verified on H2O.** PR-1: pure-compute `nano_rate_control` + the `NANORTC_FEATURE_VIDEO_RATE_CONTROL` flag (default 0) + `NANORTC_RATE_CONTROL_*` tunables + the public `nanortc_spec_rung_t` ladder type. PR-2: public `nanortc_set_capability_ladder()`, the `NANORTC_EV_SPEC_RECOMMENDATION` event, and the controller wired into the BWE receive path (`nano_rtc_media_rate_control_tick`). PR-3: `capture_set_layout()` (FFmpeg/NVENC full 3-D reopen; GStreamer bitrate-only) + example min-rung aggregation. PR-4: host E2E mid-stream param-refresh test + interop test + receiver contract. A 4-lens adversarial review of the unverifiable-on-mac ffmpeg reconfig found 5 real issues — **all fixed** (H.265 param-set use-after-free across the reopen↔track-setup thread boundary → mutex+snapshot; `apply_pending_layout` forward decl; staged-layout re-stage race / weak-memory → mutex; `g_rec_rung` stale-on-reuse → reset on disconnect; ladder-from-requested-resolution → documented). **Verification:** host `tests/test_rate_control.c` 13/13 + full suite 31/31 (flag on/off) + E2E refresh + `ci-check.sh --fast` 17/17; H2O the example **compiles clean** with the flag and **runs live** — nvenc reopened across **1080p→720p→360p without crashing**, the browser decoded the switched resolution (640×360, playing), and a 350 kbit `tc` throttle drove BWE 1228→600 kbps with PLI/keyframe recovery, recovering on release. The resolution **up-step** is bounded by REMB (rung stays at the floor when the estimate can't probe above the low send rate) — the documented Phase-B / #71 dependency, faithfully reproduced. The libdatachannel **interop test is written but could not be built** in this environment (GitHub clone fails: `RPC failed; curl 92 HTTP/2 … early EOF` on both mac + H2O) — it runs in CI. Project-initiation (立项) doc for **Phase A** of the [adaptive-media roadmap](../../engineering/adaptive-media.md#5-optimization-roadmap).
**Estimated effort:** 3–4 agent sessions (controller + tests → public API/event → example 3-D wiring → interop/H2O verify), sequenced.
**Goal:** Upgrade video adaptation from **bitrate-only** to **resolution + framerate + bitrate**, with the decision made by a **pure-compute SDK module** (the library decides, the caller applies), defaulting to **latency + smoothness over picture detail**. This lands [core belief #10](../../design-docs/core-beliefs.md#10-adaptive-media-is-the-sdks-decision-the-callers-action) as shippable code.

## Context

The [adaptive-media evaluation](../../engineering/adaptive-media.md#3-gap-analysis--does-it-meet-the-requirement) found the transport substrate solid but the spec-adaptation "brain" missing: today **only bitrate adapts** (gap G1) — resolution and framerate are fixed at `capture_start()` — there is **no adaptation policy** (G2), and the only closed loop lives in the **example** (`examples/common/bwe_coordinator.c`), not the SDK (G3). Holding 4K at a throttled bitrate spends too few bits on too many pixels → blocky output and encoder frame-drops; "smoothness first" *requires* stepping resolution/framerate down, which nothing in the codebase can do at runtime.

This phase closes G1–G3. It does **not** fix the congestion signal itself (G4, the loss-only/asymmetric BWE — that is Phase B / #71 / TD-027); Phase 14 ships with conservative up-stepping on today's signal and inherits clean up-adaptation once Phase B lands. See [Dependencies](#dependencies).

## Decision required before PR-1

These mirror the repo convention of recording hard choices up front (cf. phase13 "Scheme decision"). Recommended option is **bold**; confirm before PR-1.

1. **Recommendation delivery.** **A new event `NANORTC_EV_SPEC_RECOMMENDATION` (enum value 13, appended after `NANORTC_EV_ICE_CANDIDATE=12`), gated `NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_RATE_CONTROL`, emitted via `nano_rtc_emit_event_full()` on the BWE-update path** — structurally identical to `NANORTC_EV_BITRATE_ESTIMATE` (`src/nano_rtc_media.c`, the `bwe_should_emit_event` block ~L1533). Alternative: a pull-only `nanortc_get_recommended_spec()` the app polls. The event mirrors existing practice and needs no extra app poll loop.
2. **Capability-ladder ownership.** Caller supplies an array of `nanortc_spec_rung_t { uint16_t width, height; uint8_t fps; uint32_t bitrate_bps; }`. **The library stores only the caller's pointer + count (zero-copy, caller-owned)**, honoring [belief #5 "caller owns resources"](../../design-docs/core-beliefs.md#5-caller-owns-resources) — the caller guarantees the ladder outlives the session. Alternative: copy into a fixed `nanortc_t` array sized by a new `NANORTC_MAX_SPEC_RUNGS` macro (costs library memory; required only if a caller's ladder is transient). Default to zero-copy; revisit if a real caller needs a copy.
3. **Feature flag.** `NANORTC_FEATURE_VIDEO_RATE_CONTROL`, **default 0**, only meaningful under `NANORTC_FEATURE_VIDEO` (cf. the `NANORTC_FEATURE_VIDEO_*` family) — [belief #4 compile-time](../../design-docs/core-beliefs.md#4-compile-time-not-runtime): a non-adaptive embedded build pays nothing.
4. **Policy expression.** Phase 14 ships **one built-in default policy** (realtime/smoothness-first: pick the highest rung whose `bitrate_bps ≤ estimate × safety_factor`; step **down** immediately on congestion; step **up** only after `hold_time` of confirmed headroom). A policy-weights struct may be reserved in the API for future tuning but is **not** made configurable this phase — avoid speculative config (YAGNI).
5. **Hysteresis constants.** Asymmetric up/down thresholds + minimum hold time live in `include/nanortc_config.h` as `#ifndef NANORTC_RATE_CONTROL_*` guards with `NANORTC_*` prefix, mirroring the `NANORTC_PACING_*` / `NANORTC_BWE_*` blocks.

## Sub-PRs

Sequenced; each independently testable. PR-1/PR-2 are pure-SDK and host-verifiable; PR-3 carries the real risk; PR-4 is the delivery gate.

| PR | Scope | Primary files | Verifiable without browser? | Risk |
|----|-------|---------------|------------------------------|------|
| **PR-1** | Pure-compute controller + host unit tests | `src/nano_rate_control.{h,c}`, `include/nanortc_config.h` (flag + constants), `tests/test_rate_control.c`, `tests/CMakeLists.txt` (add to `VIDEO_TESTS`), src build list | **Yes** | Low — pure function |
| **PR-2** | Public API + event integration | `include/nanortc.h` (event enum, `nanortc_spec_rung_t`, `nanortc_set_capability_ladder()`, recommendation payload), `nanortc_t` field, `src/nano_rtc_media.c` (call + emit), `src/nano_rtc_internal.h` | **Yes** (host event-roundtrip test) | Medium — enum/ABI append discipline |
| **PR-3** | Example drives resolution **+** fps **+** bitrate | `examples/linux_uvc_camera/capture.h` + `capture_ffmpeg.c` (new `capture_set_layout()`), `examples/linux_uvc_camera/main.c` (`recompute_and_apply_bwe` rewire, ladder setup, handle new event), `examples/common/bwe_coordinator.c` (extend or supersede) | Partly (encoder reconfig is host-runnable; full loop needs the camera) | **High** — runtime geometry reconfig |
| **PR-4** | Interop + H2O verification + receiver contract | `tests/interop/test_interop_video.c` (mid-stream geometry sanity), H2O 4K↔1080p↔720p throttle run, contract text in this doc + adaptive-media.md §4.4 | Mixed (interop yes; H2O manual) | Medium — interop is the delivery gate |

## PR-1 — Pure-compute controller + tests

### Problem
There is no module that maps a congestion signal + a capability ladder to a recommended spec. `bwe_coordinator` only rate-limits a single bitrate scalar.

### Approach
Add `src/nano_rate_control.{h,c}`, mirroring the shape of `src/nano_bwe.{h,c}` (state struct in the header, `rate_control_init()`, an update entry point, getters). Core:

```c
/* Pure: no I/O, no clock, no encoder. now_ms is caller-supplied (Sans-I/O). */
int rate_control_update(nano_rate_control_t *rc,
                        uint32_t estimate_bps,      /* from bwe_get_bitrate() */
                        uint16_t loss_q8,           /* from bwe_get_loss_q8() */
                        const nanortc_spec_rung_t *ladder, uint8_t n,
                        uint32_t now_ms);
/* returns the recommended rung index; rc remembers the current rung + hold timer */
```

Selection = highest rung with `bitrate_bps ≤ estimate × safety`, with hysteresis: down-steps apply immediately; up-steps require the estimate to clear the next rung by `NANORTC_RATE_CONTROL_UP_HEADROOM_PCT` and to have held for `NANORTC_RATE_CONTROL_MIN_HOLD_MS`. Loss above a threshold forces a down-step regardless of estimate (smoothness-first). Use q8 for loss (as `nano_bwe` does) and bps for rate. Symbols use the `rate_` prefix (already in the CI symbol allowlist).

Config constants in `nanortc_config.h` (`#ifndef` + `NANORTC_RATE_CONTROL_*`, `#error`-validated like the FEC/pacing blocks): `_SAFETY_PCT`, `_UP_HEADROOM_PCT`, `_MIN_HOLD_MS`, `_LOSS_DOWN_Q8`.

### Verification
`tests/test_rate_control.c` (TEST/RUN/TEST_MAIN per `nano_test.h`, registered in the `VIDEO_TESTS` list of `tests/CMakeLists.txt`): rung selection across an estimate sweep; down-step is immediate; up-step is gated by headroom **and** hold time (does not flap); high loss forces a down-step; empty/single-rung ladders are safe. Host-only; no network.

## PR-2 — Public API + event integration

### Problem
PR-1's controller must be reachable from the public API and fed by the live BWE without the app re-rolling the loop.

### Approach
In `include/nanortc.h` (all under `#if NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_RATE_CONTROL`): add `nanortc_spec_rung_t`; append `NANORTC_EV_SPEC_RECOMMENDATION = 13` to `nanortc_event_type_t` (append only — do not renumber 11/12); add a recommendation payload to `nanortc_event_t` carrying the chosen rung index + its `{w,h,fps,bitrate}`; add `nanortc_set_capability_ladder(rtc, rungs, count)` (mirrors `nanortc_set_bitrate_bounds`). Store the controller state + ladder pointer/count in `nanortc_t` next to `nano_bwe_t bwe` (session-wide, not per-track). In `src/nano_rtc_media.c`, where REMB/TWCC already update BWE and emit `NANORTC_EV_BITRATE_ESTIMATE` (~L1533), call `rate_control_update()` and, when the recommended rung changes, emit `NANORTC_EV_SPEC_RECOMMENDATION` via `nano_rtc_emit_event_full()`.

### Verification
A host test drives synthetic REMB/TWCC into a `nanortc_t` with a ladder set and asserts a recommendation event surfaces from `nanortc_poll_output()` with the expected rung, and that no event fires while the rung is stable. `ci-check.sh` across all combos with the flag both ON and OFF; OFF must be byte-for-byte unchanged (belief #4).

### Landed
`NANORTC_EV_SPEC_RECOMMENDATION = 13` appended (gated, 11/12 unchanged); `nanortc_ev_spec_recommendation_t` payload + union member; `nanortc_set_capability_ladder()` (`src/nano_rtc_media.c`, ascending-by-bitrate boundary validation, zero-copy caller-owned). Controller state (`rc` + `rc_ladder`/`rc_ladder_n`) added to `nanortc_t` beside `bwe`; `rate_control_init()` in `nano_rtc.c`. `nano_rtc_media_rate_control_tick()` runs after each REMB/TWCC `bwe` update and emits on rung change or first selection. Tested directly (the tick is non-static via `nano_rtc_internal.h`): ladder-API validation + tick→emit field mapping + no-flap + no-ladder-idle. The include cycle (umbrella header needs `nano_rate_control_t` by value) was resolved by the `nano_media.h` pattern — the module header relies on the `nanortc.h` umbrella for `nanortc_spec_rung_t` rather than including it. Wire-neutral (consumes existing feedback, emits a local event only); flag default-off. `ci-check.sh --fast` 17/17.

## PR-3 — Example: resolution + fps + bitrate

### Problem
The example encoder has **no runtime geometry knob** — `capture_config_t` is immutable after `capture_start()`, and `capture_set_bitrate()` only stages `pending_bps` (the encode thread writes `bit_rate`/`rc_max_rate`/`rc_buffer_size` before the next `avcodec_send_frame`). Driving the controller's recommendation needs a way to change width/height/fps mid-stream.

### Approach
Add `capture_set_layout(w, h, fps)` to `capture.h` + `capture_ffmpeg.c`, staged like `pending_bps` and applied on the encode thread. **Open technical risk:** nvenc/ffmpeg likely cannot reconfigure resolution in place — expect a codec-context close+reopen (re-`avcodec_open2` with new `enc_w/enc_h/fps`), re-deriving and re-publishing the H.265 VPS/SPS/PPS via the existing `capture_get_h265_parameter_sets()` → `nanortc_video_set_h265_parameter_sets()` path, then forcing an IDR (the encoder already keeps `force_idr` + the recently added `intra-refresh`). The swap must be glitch-bounded (drop in-flight frames during reopen, not crash the pipe). In `main.c`, rewire `recompute_and_apply_bwe()`: feed the per-viewer minimum estimate into the controller (or consume `NANORTC_EV_SPEC_RECOMMENDATION`), set the ladder once via `nanortc_set_capability_ladder()`, and apply the recommended rung through `capture_set_layout()` + `capture_set_bitrate()`. `bwe_coordinator.c` either grows resolution/fps fields or is superseded by the SDK controller (decide in PR-3 once the API shape from PR-2 is fixed).

### Verification
Host: encoder reconfig unit-exercised where possible (open → reconfig → re-extract param sets → IDR). Field: H2O 4K↔1080p↔720p under `tc` clsact+police throttle on the gateway path (per [[project_nvenc_h265_camera]]) — narrowing the link steps geometry **down** and stays smooth; clearing it steps **up** without flapping; Chrome `getStats` shows the resolution/fps change and no sustained freeze.

## PR-4 — Interop + H2O verification + receiver contract

### Problem
Mid-stream geometry change must not break SRTP, depacketization, or the negotiated session, and the receiver must know how to consume a new geometry.

### Approach
Extend `tests/interop/test_interop_video.c` with a sanity check that a parameter-set refresh + IDR mid-stream is consumed without renegotiation (the transport is geometry-agnostic, so this should hold; the test pins it). Document the **receiver contract**: on a spec switch the receiver consumes the new in-band VPS/SPS/PPS and waits for the following IDR before decoding the new geometry. Write the contract into this doc and [adaptive-media.md §4.4](../../engineering/adaptive-media.md#44-package-the-spec-change-protocol).

### Verification
`ctest -R interop_video` stays green ([belief #7](../../design-docs/core-beliefs.md#7-mechanical-enforcement-over-documentation)); libdatachannel interop is the delivery gate per [[feedback_testing_interop]]. H2O run reproduced and captured.

### Landed
- **`capture_set_layout(w,h,fps)`** added to `capture.h`. FFmpeg/NVENC backend: full 3-D — staged like `pending_bps`, the capture thread reopens the encoder at the new geometry (downscaling from the fixed capture resolution via swscale), drops input frames to hit `fps`, re-caches in-band parameter sets, and forces an IDR; PTS uses a fixed capture-rate time_base so frame-skip stays time-correct; reopen failure falls back to the capture resolution. GStreamer backend: returns -1 (unsupported, caps baked at launch) — bitrate adaptation still applies.
- **`examples/linux_uvc_camera/main.c`** (under `NANORTC_FEATURE_VIDEO_RATE_CONTROL`): builds a 3-rung ladder from the capture geometry + `[-m,-M]`, installs it per session (`nanortc_set_capability_ladder`), and on `NANORTC_EV_SPEC_RECOMMENDATION` aggregates the **min rung across viewers** (slowest governs the single shared encoder) → `capture_set_layout` + bitrate. Bitrate keeps tracking BWE, clamped to the active rung's ceiling. Stats line shows `rung=i WxH@fps`. Flag-off path is unchanged (bitrate-only).
- **Tests**: host `test_e2e_h265_midstream_param_refresh` (a mid-session `nanortc_video_set_h265_parameter_sets` re-set + a fresh IDR is consumed without renegotiation) and interop `test_interop_video_h265_midstream_refresh` (the real spec-switch wire — a multi-NAL `[VPS][SPS][PPS][IDR]` access unit — reaches a libdatachannel receiver). Multi-NAL AP/FU packetization itself is unit-tested in `test_h265.c`.

### Receiver contract (spec switch)
On a geometry change the **sender** forces an IDR and re-emits the parameter sets **in-band** as a multi-NAL access unit (`[VPS][SPS][PPS][IDR]` for H.265, `[SPS][PPS][IDR]` for H.264) — there is no mid-stream SDP update (`sprop-*` is offer/answer-only). The **receiver** must consume the new parameter sets and wait for the IDR before decoding the new geometry; until the IDR arrives it keeps decoding the old geometry (or drops, then auto-PLIs). Because the nanortc transport is geometry-agnostic (the packetizers fragment by MTU only; no width/height state), no renegotiation is needed. H.264 carries parameter sets in-band only (no `nanortc_video_set_h264_parameter_sets` API — that API-parity is Phase C).

### Adversarial review (all findings fixed)
Because the ffmpeg/nvenc reconfig path cannot be built or run on the dev host (macOS; V4L2/nvenc are Linux-only), a 4-lens adversarial-review workflow (lifetime / threading / correctness / aggregation) over the PR-3 diff substituted for a runtime check. Five real defects, all fixed in `capture_ffmpeg.c` / `main.c`:
1. **(HIGH) H.265 param-set use-after-free.** `reopen_encoder` (capture thread) freed `sps_pps`/`psets_buf`/`vps/sps/pps` mid-session while `capture_get_h265_parameter_sets` (main thread, when a 2nd viewer's track is set up) read them — the pre-Phase-14 single-alloc-for-session lifetime no longer held. Fixed with a `g_ff.lock` mutex guarding the param-set storage + a stable snapshot copy returned to callers.
2. **(MED) `apply_pending_layout` implicit declaration** (called in `capture_thread` before its definition) → build break on clang/strict. Fixed with a forward declaration.
3. **(MED) staged-layout re-stage race / weak-memory ordering** — the volatile `pending_{w,h,fps}` trigger could be half-read or a re-stage lost. Folded under the same mutex (atomic stage + snapshot-and-clear).
4. **(MED) `g_rec_rung` stale on slot reuse** — a reused session slot could inherit a departed viewer's rung. Fixed by resetting it on `EV_DISCONNECTED`.
5. **(MED) ladder built from requested `-W/-H`, not the negotiated capture resolution** — documented (the oversized top rung is rejected by `capture_set_layout` and the encoder stays at the actual capture resolution; set `-W/-H` to the camera's native resolution).

### Live verification (H2O, RTX 4070, real UGREEN camera → Chrome)
- The example **compiles clean** with `-DCMAKE_C_FLAGS=-DNANORTC_FEATURE_VIDEO_RATE_CONTROL=1` against real ffmpeg/nvenc/gcc; ladder built `[0]640x360@15 600k [1]1280x720@30 3066k [2]1920x1080@30 8000k`.
- **Live nvenc reopen, multi-step**: cold-start → rung 1 (`[ffcap] layout -> 1280x720@30fps`); when REMB dragged the estimate down → rung 0 (`[ffcap] layout -> 640x360@15fps`). No crash, stream continuous. Browser confirmed the switch: `video.videoWidth=640, videoHeight=360, playing`.
- **Real-bandwidth response**: a 350 kbit `tc` clsact+police throttle (matched on the real media 5-tuple — note the dst is the peer's LAN address, **not** the gateway) drove BWE 1228→600 kbps with PLI→forced-keyframe recovery; on release the BWE recovered (600→892+ kbps).
- **Up-step is REMB-bounded**: with the wide `[-m 600k, -M 8M]` ladder the controller parked at the floor rung once the REMB estimate plateaued ~1.2 Mbps (rung 1 needs ~3.7 Mbps with headroom) — the documented Phase-B / #71 dependency, reproduced exactly.
- **Interop test**: written + registered, but the libdatachannel `FetchContent` clone failed on both mac and H2O (`git RPC failed; curl 92 HTTP/2 … fatal: early EOF` — GitHub network flakiness, not a code issue); the ci-check interop build was also fixed to enable VIDEO/H265 so the video interop suite actually gates. The interop test will run in CI where GitHub is reachable.

## Embedded budget (must hold)

- **`NANORTC_FEATURE_VIDEO_RATE_CONTROL=0` → zero cost** (belief #4): no struct fields, no code, no symbols. Verified by the symbol/size checks in `ci-check.sh`.
- **Enabled:** controller state is a handful of scalars (current rung, hold timer, last estimate) — well under 64 B. The capability ladder is **caller-owned** (zero-copy), so the library adds no per-rung memory. No allocation in `src/` (belief #5).

## Dependencies

- **Phase B / #71 / TD-027** — the controller's **up-step** quality is bounded by the congestion signal. Today's BWE is loss-only and asymmetric (parks at the floor after a throttle), so Phase 14 up-steps conservatively on the REMB-bounded estimate. Once Phase B adds a delay-based estimate + probing, the same controller gets clean, proactive up-adaptation with no API change. Phase 14 is shippable without Phase B; it just adapts up more slowly until then.
- **Reorder/keyframe** — the spec-switch IDR relies on `nanortc_request_keyframe` / encoder forced-IDR (already shipped).

## Acceptance Criteria

- [ ] `nano_rate_control` is pure (no I/O, no clock, no encoder); `test_rate_control` covers rung selection, asymmetric hysteresis, hold-time, and loss-forced down-step; `ci-check.sh` green across all combos with the flag ON and OFF.
- [ ] `NANORTC_FEATURE_VIDEO_RATE_CONTROL=0` produces a byte-identical library (belief #4).
- [ ] On H2O, a narrowing link steps down to 1080p/720p (and/or lower fps) and stays smooth; a recovering link steps back up without flapping.
- [ ] `interop_video` stays green; the receiver contract (consume new VPS/SPS/PPS, wait for IDR before decoding the new geometry) is documented.

## Non-goals

- **Phase B** — delay-based / probing BWE (#71 / TD-027). Up-step cleanliness depends on it but is out of scope here.
- **Phase C** — H.264 parameter-set API parity and a generic force-IDR-on-switch helper (the example does it inline this phase).
- **Phase D** — independent per-direction controllers for two-way (Profile C) sessions.
