# Phase 12: Receive-path robustness for embedded real-time video

**Status:** PR-1 (auto-PLI on receive loss) implemented + full CI matrix / ASan / libdatachannel interop green. PR-2 / PR-3 pending.
**Estimated effort:** 2–3 agent sessions.
**Goal:** Make NanoRTC a robust *receiver* of real-time video (viewer / two-way intercom) on lossy, reordering wireless links — today a single lost or reordered packet silently breaks a frame with no recovery.

## Context

The send path is now strong (Phase 9 BWE → Phase 11 pacer + #67 admission, plus
sender-side NACK retransmit + PLI handling). The **receive** path was the weak
half: inbound video RTP is depacketized immediately with **no sequence-gap
detection**, the depacketizer silently drops incomplete frames, and the
`NANORTC_EV_MEDIA_DATA.contiguous` flag was hardcoded `true`. So a receiver that
loses a reference packet stays **frozen** until the application happens to call
`nanortc_request_keyframe()` itself. For a video doorbell screen or a two-way
intercom, that is the dominant visible failure.

This phase adds the three standard receiver-robustness primitives, sequenced
lowest-risk-first.

## Sub-PRs

| PR | Topic | Primary files | Status |
|----|-------|---------------|--------|
| **PR-1** | Auto-PLI keyframe recovery on detected forward-gap loss + accurate `contiguous` flag | `src/nano_rtc_media.c`, `src/nano_media.h`, `include/nanortc{,_config}.h`, `tests/test_e2e.c` | **Implemented** |
| **PR-2** | Small bounded video reorder buffer (heal benign WiFi reordering before it reaches the gap detector / depacketizer; tight latency budget) | `src/nano_reorder.{c,h}`, `src/nano_rtc_media.c`, `src/nano_media.h` | **Implemented** |
| **PR-3** | Receiver-side NACK generation (gap → RTPFB NACK, paired with the existing sender retransmit) | `src/nano_rtc_media.c`, `src/nano_rtcp.{c,h}` | **Implemented** |

## PR-1 design (implemented)

On each inbound video RTP packet, compare the seq against the per-track
`recv_last_seq` using `(int16_t)(seq - last)` (RFC 3550 §5.1 16-bit wrap):

- `> 1` → **forward gap** (≥1 packet lost): advance, flag loss, and emit a
  debounced RTCP PLI (`NANORTC_VIDEO_PLI_MIN_INTERVAL_MS`, default 1000 ms) so
  the sender re-sends a keyframe and the decoder resyncs.
- `== 1` in order; `<= 0` late/duplicate (reordered) — ignored, `recv_last_seq`
  not regressed.

The delivered frame's `contiguous` flag now reflects whether a gap preceded it
(`!recv_lost_pending`, cleared per delivered frame). PLI rate-limiting is **debounce
only** (no `awaiting_keyframe` latch) so a lost keyframe self-heals on the next
interval. Feature-gated by `NANORTC_FEATURE_VIDEO_AUTO_PLI` (default 1); the PLI
emit is shared with `nanortc_request_keyframe()` via a new `rtc_emit_pli()` helper.
Counter: `stats_auto_pli_sent` (surfaced in the camera `/debug` endpoint pattern).

**Known limitation (v1):** without PR-2's reorder buffer, packet reordering looks
like a gap; the debounce caps the cost to one keyframe request per interval.

**Found & fixed alongside PR-1 (both → TD-024 resolved):**
1. **PLI mapping (TD-024).** The inbound PLI→`EV_KEYFRAME_REQUEST` mapping keyed
   off the RTCP *sender* SSRC via `ssrc_map`, so it named the wrong track on
   multi-track sessions and produced *no* event for a sendonly camera. Fixed:
   `rtcp_parse` now extracts the PSFB media-source SSRC (`psfb_media_ssrc`), and
   the handler matches it against each outbound video track's `m->rtp.ssrc`.
2. **PLI lifetime bug.** `rtc_emit_pli()` (shared with `nanortc_request_keyframe`)
   built the PLI in a **stack buffer** and enqueued a pointer to it — clobbered
   before `poll_output()` when auto-PLI fires mid-receive (garbage PLI on the
   wire; latent in `nanortc_request_keyframe` too). Fixed with a persistent
   `rtc->rtcp_fb_buf` (`NANORTC_RTCP_FB_BUF_SIZE`).

## Verification (PR-1)

- `tests/test_e2e.c::test_e2e_video_auto_pli_on_loss`: a gap-injecting relay
  (`e2e_relay_drop_rtp`) drops a video packet; asserts the receiver auto-PLIs once
  on the gap, stays silent in-order, debounces a second gap within the interval,
  re-fires after it (via `stats_auto_pli_sent`), **and** the PLI round-trips to
  the sendonly sender as `EV_KEYFRAME_REQUEST` carrying the correct mid (proves
  both the mapping and lifetime fixes).
- `./scripts/ci-check.sh`: **42/42** (7 combos × openssl/mbedtls + arch + format +
  ASan + libdatachannel interop). Auto-PLI-disabled build (`=0`) green — fallback
  proven.

## PR-2 design (implemented)

A bounded, Sans-I/O reorder buffer (`src/nano_reorder.c`) sits in front of the
video depacketizer when `NANORTC_FEATURE_VIDEO_REORDER` is on (**opt-in,
default 0**). It holds inbound RTP payloads in a window indexed by
`seq & (SLOTS-1)` and releases them strictly in order, so:

- benign reordering is **healed** before it breaks FU reassembly (no loss), and
- the loss signal becomes **precise**: the buffer's *skip* is the only loss
  source, so a reorder no longer reads as a gap and auto-PLI / `contiguous`
  stop false-firing on reordering links.

Release / progress rules (all bounded):
- in-order packet → release immediately, `lost=false`;
- a missing `next_seq` is **skipped** (and the next release flagged `lost=true`)
  once a buffered packet has waited `NANORTC_VIDEO_REORDER_MAX_WAIT_MS` — the
  hard latency cap, enforced on packet arrival (continuous for a live stream);
- a packet farther than `NANORTC_VIDEO_REORDER_SLOTS` ahead **force-advances**
  the window, declaring the jumped span lost;
- `seq < next_seq` (late / duplicate) is dropped.

The receive path was refactored to a shared `rtc_video_deliver(payload, …,
lost)` helper; with reorder on it runs `reorder_push` → drain `reorder_pop`,
with reorder off it keeps the raw forward-gap detection (PR-1). Cost: `SLOTS ×
NANORTC_MEDIA_BUF_SIZE` per video track (~10 KB at the default 8 slots) plus up
to `MAX_WAIT_MS` receive latency — hence default-off so a send-only camera pays
nothing.

### PR-2 adversarial review (4 confirmed findings, all fixed)

A 3-dimension review (logic / integration / safety) × per-finding verification
surfaced 4 real, reorder-only defects (default builds unaffected); all fixed
before sign-off:

1. **Multi-emit aliasing (high).** The original drain ran the `reorder_pop`
   loop inside `handle_input`, queuing ≥2 `EV_MEDIA_DATA` events that all alias
   the single per-track depkt buffer → all but the last corrupted, defeating the
   feature. **Fixed**: the drain moved to a **poll-time produce**
   (`nano_rtc_media_reorder_produce`, called from `nanortc_poll_output`) that
   releases exactly one NAL per poll, so each event is consumed before the next
   reuses the depkt buffer — mirroring the pacer pattern.
2. **SSRC-change blackhole (high).** A mid-session SSRC change reused the track
   without resetting `next_seq`, dropping the new (RFC-random) stream via the
   `diff<0` late-drop. **Fixed**: the PT-match discovery branch now resets the
   reorder buffer + depacketizer on a detected SSRC change.
3. **No timer flush (medium).** The skip cap was arrival-driven only, so a held
   frame stalled past the cap on an idle/low-fps stream. **Fixed**:
   `reorder_next_timeout_ms` is folded into `nanortc_next_timeout_ms`, and the
   poll-time produce enforces the cap on the timer-tick wake.
4. **ESP-IDF build (high).** `nano_reorder.c` was missing from the ESP-IDF
   source list → link error if enabled on the primary embedded target.
   **Fixed**: added to the `CONFIG_NANORTC_FEATURE_VIDEO` branch.

(Refuted: a stale-slot `should_skip` spin — already hardened pre-review; and an
"in-order reorder drops a late NAL" — intended by-design behavior.)

## Verification (PR-2)

- `tests/test_reorder.c` (isolation, 6 cases): in-order passthrough, reorder
  heal (swapped pair delivered in order, **no loss**), late-drop, timeout-skip
  (loss after the cap), far-future force-advance (loss), and 16-bit seq
  wraparound incl. a reorder across `0xFFFF→0x0000`.
- `tests/test_e2e.c::test_e2e_video_reorder_heals_swap` (integration): two
  distinct single-NAL frames delivered REVERSED are released **in order with
  correct, non-aliased bytes** (proves fix 1) and a mid-stream SSRC change with a
  far-behind seq is **delivered, not blackholed** (proves fix 2).
- `./scripts/ci-check.sh` gains a **MEDIA + VIDEO_REORDER** build+test step (the
  default matrix never compiles the reorder path); **44/44** total incl. ASan +
  libdatachannel interop. Default (reorder-off) matrix unchanged — no regression.

## PR-3 design (implemented)

Receiver-side NACK (`NANORTC_FEATURE_VIDEO_NACK_RX`, **opt-in, default 0**)
completes the retransmit loss-recovery loop (the sender's pkt_ring NACK-retransmit
already existed). On a forward RTP sequence gap, the receiver emits an RTCP
Generic NACK (RFC 4585 §6.2.1) — PID = first lost seq, BLP = bitmask of the next
up-to-16 — asking the sender to retransmit from its pkt_ring. Cheaper than a
keyframe for small losses; recovers within ~1 RTT.

- `rtcp_generate_nack` gained a `blp` parameter (was hardcoded BLP=0).
- Gap detection is self-contained (`recv_nack_seq`, independent of the
  reorder/auto-PLI trackers) and resets on an SSRC change.
- The NACK is built into a dedicated `nack_buf` (distinct from the PLI's
  `rtcp_fb_buf`, since one gap pass can emit both). Counter: `stats_nack_sent`.
- **Best paired with the reorder buffer**: the buffer holds the gap (up to
  `NANORTC_VIDEO_REORDER_MAX_WAIT_MS`) so the retransmit can fill it for in-order
  recovery; tune the cap ≥ link RTT. v1 NACKs each new gap once (retry is a
  follow-up).

## Verification (PR-3)

- `tests/test_e2e.c::test_e2e_video_nack_recovers_drop` (integration, REORDER +
  NACK_RX): a packet is dropped on the wire (kept in the sender's pkt_ring); the
  receiver detects the gap and NACKs (`stats_nack_sent`), the sender retransmits,
  and the reorder buffer fills the gap so the frame is **recovered in order**.
- `tests/test_rtcp.c`: `rtcp_generate_nack` BLP parameter.
- `./scripts/ci-check.sh`'s opt-in step now builds+tests **MEDIA + REORDER +
  NACK_RX** together (the natural pairing); **44/44** incl. ASan + interop.
  Default builds (NACK off) unchanged.

## Out of scope (this phase's tail + beyond)

- PR-2 reorder buffer, PR-3 receiver NACK (above).
- TD-024 PLI media-source-SSRC mapping fix; TD-023 NACK retransmit scratch ring.
- FEC (ULPFEC/RED) — separate phase.
