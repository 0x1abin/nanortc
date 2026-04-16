# Phase 9: BWE Perception for IoT Camera / Intercom

**Status:** Active (PR-1…PR-5 landed, PR-6 = this doc).
**Estimated effort:** 1–2 agent sessions (six self-contained PRs).
**Goal:** Give IoT camera / intercom applications sufficient congestion-control signal (TWCC, plus structured stats and tunables) so the application layer can drive its hardware encoder's rate-control knobs without NanoRTC itself running a pacer or enforcing a target rate.

## Context

NanoRTC shipped Phase 3 with REMB + EMA smoothing, an event (`NANORTC_EV_BITRATE_ESTIMATE`), and the `nanortc_get_estimated_bitrate()` query. That is sufficient when the remote end sends REMB, but:

- **REMB is being retired.** Chrome / libwebrtc / libdatachannel default to Transport-wide Congestion Control (TWCC, RFC 8285 header extension + RTCP PT=205 FMT=15 feedback). Against a modern browser, NanoRTC previously had no bandwidth signal at all.
- **IoT camera firmware cannot recompile to adjust BWE bounds.** A device discovering at runtime that its hardware encoder only delivers 1.5 Mbps needs a setter API.
- **App-layer encoder drivers need more than one number.** They want the estimated bitrate, the actual wire rate they are producing, the frame rate, and the reported loss fraction — all in one stats snapshot.
- **Event payload was single-source.** Consumers that want to react differently to "up" vs "down" or to distinguish TWCC-driven from REMB-driven updates had to diff the numbers themselves.

Phase 9 closes these four gaps **without** adding pacers, delay-based GCC, or any active rate enforcement inside the library. NanoRTC stays Sans I/O; the app remains in charge of feeding its encoder.

## Sub-PRs

| PR | Topic | Primary files | Status |
|---|---|---|---|
| **PR-1** | TWCC feedback parser (RFC draft-holmer-rmcat-twcc-01 §3.1) + unit tests | `src/nano_twcc.{h,c}`, `tests/test_twcc.c` | Landed |
| **PR-2** | SDP `a=extmap` negotiation + RTP one-byte header extension writing | `src/nano_sdp.{h,c}`, `src/nano_rtp.{h,c}`, `src/nano_rtc.c`, `include/nanortc_config.h` | Landed |
| **PR-3** | TWCC signal → BWE loss-based controller + event payload `direction` / `source` | `src/nano_bwe.{h,c}`, `src/nano_rtc.c`, `include/nanortc.h` | Landed |
| **PR-4** | Runtime setter API (bounds / initial / event threshold) | `include/nanortc.h`, `src/nano_bwe.{h,c}`, `src/nano_rtc.c` | Landed |
| **PR-5** | Stats extension (`send_bitrate_bps`, `send_fps_q8`, `fraction_lost`, `estimated_bitrate_bps`) + 1 s rate window + RTCP RR/SR report-block parsing | `include/nanortc.h`, `src/nano_media.{h,c}`, `src/nano_rtcp.{h,c}`, `src/nano_rtc.c` | Landed |
| **PR-6** | Docs (this plan, design-doc BWE section, RFC index, memory-profiles) | `docs/**` | In progress |

---

## PR-1 — TWCC parser core

Added `src/nano_twcc.{h,c}` with a summary-plus-optional-callback API. Parser covers all three chunk types (run-length, 1-bit status vector, 2-bit status vector), tracks `received_count`, and decodes receive deltas without copying.

Guarded by `NANORTC_FEATURE_VIDEO` only — no separate feature flag so the first-wave footprint stays small. Max packets per feedback is capped by `NANORTC_TWCC_MAX_PACKETS_PER_FB` (default 128) to bound stack usage.

22 unit tests; exercises three chunk formats, seq wrap, truncation, count-overflow, empty feedback, and a hand-crafted RFC-style byte vector.

## PR-2 — SDP + RTP wiring

- SDP `a=extmap:<id> <URI>` parser/generator in `src/nano_sdp.c`. Offerer side uses `NANORTC_TWCC_EXT_ID` (default 3, Chrome's convention); answerer side echoes whatever ID the offerer advertised. Non-TWCC extmap URIs are ignored so IDs allocated by the remote don't collide.
- `nano_rtp_t` grows two fields (`twcc_ext_id`, `twcc_seq`). `rtp_pack()` emits a `0xBEDE` one-byte header extension (RFC 8285 §4.2) when the ID is in `1..14`, adding exactly 8 bytes per packet and auto-incrementing the transport-CC sequence counter.
- `rtc_apply_negotiated_media()` wires `ml->twcc_ext_id` into `m->rtp.twcc_ext_id` for video m-lines once the offer/answer exchange finishes.
- `NANORTC_MEDIA_BUF_SIZE` comment updated to reflect the 8-byte extension overhead (still fits inside the existing +32 headroom).

Existing `rtp_unpack()` already skips arbitrary RTP header extensions, so no receive-side change was needed.

## PR-3 — BWE loss controller + richer event

- `bwe_on_twcc_loss(bwe, loss_fraction_q8, now_ms)` implements a legacy-libwebrtc flavour controller:
  - `loss_q8 < 5` (< ~2 %): multiply by 1.08 (grow 8 %).
  - `loss_q8 in [5, 25]` (2 %–10 %): hold.
  - `loss_q8 > 25` (> ~10 %): multiply by `(512 − loss_q8) / 512`.
- Result is clamped to `[MIN_BITRATE, MAX_BITRATE]`, blended with the existing estimate through the same `NANORTC_BWE_EMA_ALPHA` EMA as REMB, and shared via the same event channel.
- Deliberately no delay-based / trendline controller in this phase; loss is enough for an IoT camera and keeps the code auditable.
- `NANORTC_EV_BITRATE_ESTIMATE` payload grows `direction` (`STABLE` / `UP` / `DOWN`) and `source` (`REMB` / `TWCC_LOSS`). Existing consumers that stop at the old `prev_bitrate_bps` field are untouched.
- RTCP dispatch in `nano_rtc.c` adds a branch for `RTPFB FMT=15` that runs the parser and feeds loss into BWE.

## PR-4 — Runtime setter API

Three new public calls, all additive, all no-ops at zero cost when unused:

```c
int nanortc_set_bitrate_bounds(nanortc_t *, uint32_t min_bps, uint32_t max_bps);
int nanortc_set_initial_bitrate(nanortc_t *, uint32_t bps);
int nanortc_set_bwe_event_threshold(nanortc_t *, uint8_t pct);
```

Runtime values shadow the compile-time `NANORTC_BWE_*` macros when non-zero. `set_initial_bitrate` only applies before the first feedback packet arrives; after that the estimate is tracking live feedback and further calls no-op. `set_bitrate_bounds` also clamps the current estimate immediately so subsequent `nanortc_get_track_stats()` is consistent.

## PR-5 — Stats extension

`nanortc_track_stats_t` grows four fields at the tail so older binaries reading the struct only see zeros in the new region:

| Field | Source | Update frequency |
|---|---|---|
| `estimated_bitrate_bps` | `nano_bwe_t.estimated_bitrate` (alias of legacy `bitrate_bps`) | Every feedback |
| `send_bitrate_bps` | Outgoing post-SRTP wire bytes × 8 over 1 s | Rolls every second |
| `send_fps_q8` | `nanortc_send_video()` call count × 256 (Q8.8) over 1 s | Rolls every second |
| `fraction_lost` | Report-block `fraction_lost` from last parsed RTCP RR or SR | Per received report |

Backed by `nano_rate_window_t` — a single-bucket integer ring that holds `prev_bps` / `prev_fps_q8` from the previous completed second and accumulates the current second. No floating point, ~28 bytes per track.

RTCP parser now extracts the first report block from RR (offset 8) and SR (offset 28, when `RC>=1`). `nano_rtcp_info_t` gets `rb_valid`, `rb_source_ssrc`, `rb_fraction_lost`.

## Deliberately out of scope

- TWCC feedback **sending** (double-ended intercom: Phase 10 candidate).
- Delay-based / GCC-style controller.
- Pacer, frame dropping, active rate enforcement.
- Split `NACK` / `PLI` / `FIR` events. `NANORTC_EV_KEYFRAME_REQUEST` is a sufficient signal for the hardware encoders IoT targets ship with.
- Audio TWCC. Wiring is video-only for now; adding audio would need the BWE consumer to multiplex two streams.
- TMMBR / TMMBN (RFC 5104) — browsers don't use them.

## Verification

- `ctest --test-dir build` — 22/22 targets, 100 % pass. Includes the new `test_twcc` suite (22 cases), 5 extra `test_rtp` cases for the TWCC extension layout, 5 extra `test_sdp` cases for extmap parsing/generation/roundtrip, 10 extra `test_bwe` cases for TWCC loss + runtime setters, 5 extra `test_media` cases for the rate window, 2 extra `test_rtcp` cases for report-block extraction.
- `./scripts/ci-check.sh` — feature matrix across all six profiles. `NANORTC_FEATURE_VIDEO=OFF` build confirms `nano_twcc.c` is not compiled and `test_twcc` is not built.
- Follow-up — **pending before merge**: browser interop snapshot against Chrome 146 (one `examples/browser_interop` run showing TWCC feedback arriving and the event's `source` field being `NANORTC_BWE_SRC_TWCC_LOSS`).

## Memory impact (ARM32, per track)

| Addition | Bytes |
|---|---|
| `nano_rtp_t.twcc_{ext_id,seq}` | +4 |
| `nano_bwe_t.{twcc_count, last_source, runtime_*}` | +16 (shared, not per track) |
| `nanortc_track_t.rate_window` | +20 |
| `nanortc_track_t.fraction_lost` | +1 (padded to +4) |
| `nano_rtcp_info_t.{rb_*}` | stack only |
| `nanortc_track_stats_t` new tail | +12 (caller buffer) |

Worst-case per-track increase: **~28 bytes**. Shared BWE growth: **~16 bytes**. No new heap allocations; stack footprint dominated by `statuses[NANORTC_TWCC_MAX_PACKETS_PER_FB]` (default 128 bytes, on the parser call stack only).
