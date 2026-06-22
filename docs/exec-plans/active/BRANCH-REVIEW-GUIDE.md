# Review guide — branch `feat/video-bwe-pacer`

A navigator for reviewing/landing this branch (Phases 11–12 + fixes). ~1600
lines across 21 files. Everything is **opt-in / default-off except the pacer +
auto-PLI**, so default builds are unchanged. Suggested review order below;
each phase is independently understandable.

## TL;DR for the reviewer

- **What it adds:** the embedded-video **loss-recovery loop** — send pacing,
  auto-PLI keyframe recovery, a receive reorder buffer, and receiver NACK — plus
  two latent-bug fixes (TD-024, TD-025).
- **Risk surface:** the send hot path (#67 admission) for the pacer; the video
  receive path for PR-1/2/3. Both covered by adversarial review + CI.
- **Gate:** `./scripts/ci-check.sh` → **44/44** (7 combos × 2 crypto + ASan +
  REORDER+NACK step + libdatachannel audio+video interop). Run it.
- **Not done (needs you):** real-link/browser smoke; FEC (designed, Phase 13).

## Review order (commit-sized chunks)

| # | Topic | Key files | What to check |
|---|-------|-----------|---------------|
| 1 | **Pacer** (Phase 11) | `nano_rtc_media.c` (pacer_*), `nano_rtc.c` (poll/timeout), `nano_pacer_t` in `nanortc.h` | Token-bucket math; the admission/aliasing invariant change (does it still guarantee whole-frame-or-WOULD_BLOCK?); the `MAX_QUEUE_MS` latency cap. Review notes in `phase11-send-pacing.md`. |
| 2 | **TD-024 fix** | `nano_rtcp.{c,h}` (psfb_media_ssrc), `nano_rtc_media.c` (PLI handler), `rtc_emit_pli` | PLI now maps via media-source SSRC (not sender SSRC); PLI built into persistent `rtcp_fb_buf` (was a stack buffer — the real bug). |
| 3 | **auto-PLI** (PR-1) | `nano_rtc_media.c` (receive branch), `nano_media.h` (recv_* state) | Forward-gap → debounced PLI; `contiguous` flag accuracy. |
| 4 | **Reorder buffer** (PR-2) | `nano_reorder.{c,h}` (new), `nano_rtc_media.c` (produce path), `nano_rtc.c` (poll/timeout) | The bounded window (wraparound, force-advance, skip cap); **the poll-time producer** (one NAL per poll — fixes the multi-emit aliasing the review found). `phase12` doc has the 4 review findings. |
| 5 | **TD-025 fix** | `nano_rtc_media.c` (audio produce), `nano_rtc.c` | Audio jitter drain moved to a poll-time producer (same aliasing class as PR-2). |
| 6 | **Receiver NACK** (PR-3) | `nano_rtcp.{c,h}` (blp), `nano_rtc_media.c` (rtc_emit_nack + gap) | NACK PID+BLP on gap; dedicated `nack_buf`; SSRC-change reset. |
| 7 | Tests / fuzz / docs | `test_reorder.c`, `test_video_pacing.c`, `fuzz_reorder.c`, e2e tests, `phase11/12/13` + `deployment-profiles.md` | Coverage of each feature. |

## How to verify (no special hardware)

```bash
./scripts/ci-check.sh                 # 44/44 — the delivery gate
# Opt-in features (not in the default matrix) are built+tested in the
# "MEDIA + REORDER + NACK_RX" step inside ci-check.
```

Key e2e proofs: `test_e2e_video_send_admission` (pacer admission preserved),
`test_e2e_video_auto_pli_on_loss` (auto-PLI + the TD-024 round-trip),
`test_e2e_video_reorder_heals_swap` (reorder heals a swap, non-aliased + SSRC
change), `test_e2e_video_nack_recovers_drop` (full NACK loop: drop → NACK →
retransmit → reorder-fill → recover).

## Browser smoke checklist (the part that needs you)

1. Build `examples/esp32_camera` (or `linux browser_interop`), enable
   `NANORTC_FEATURE_VIDEO_PACING` (on by default).
2. Camera → Chrome: confirm no stutter; watch `/debug` — `pace_catchup` rare on
   a healthy link; `stats_pkt_ring_overrun == 0`.
3. (Viewer/two-way) enable `NANORTC_FEATURE_VIDEO_REORDER` + `_NACK_RX`
   (see `deployment-profiles.md`); induce loss; confirm recovery without freeze.

## Tracked debt — resolved in this branch

- **TD-023** (resolved): the NACK-retransmit non-contiguous pkt_ring slot vs the
  entry-count admission gate — pre-existing, latent (didn't manifest in the
  bundled run-loops), bounded — is **fixed in this branch**. The generic-NACK
  answer now copies the matched packet into a dedicated `nack_retx_buf` scratch
  ring and enqueues the copy, so a later `send_video()` wrapping `pkt_ring` can no
  longer corrupt an in-flight retransmit. Revert-verified in
  `test_e2e_video_nack_recovers_drop`; tracker entry marked RESOLVED.

## After landing

`docs/exec-plans/active/phase13-fec.md` is the next phase (FEC) — **decision
required**: scheme (RED+ULPFEC recommended) + whether to start before or after
this branch lands.
