# Phase 3.5: H.265/HEVC Video Support

**Status:** Active — PR-1 (module + tests + fuzz) ready for review; PR-2 and PR-3 pending.
**Estimated effort:** 2–3 agent sessions total (split across 3 independent PRs).
**Authoritative spec:** [RFC 7798](https://www.rfc-editor.org/rfc/rfc7798) — "RTP Payload Format for High Efficiency Video Coding (HEVC)" (December 2016).

## Goal

Bidirectional H.265 send + receive in NanoRTC, compatible with Chrome M125+ / Safari 17+ WebRTC H.265 peers, gated behind a sub-feature flag (`NANORTC_FEATURE_H265`) so flash-constrained IoT targets can opt out.

## Context

Phase 3 shipped H.264 video (RFC 6184) with browser interop, fuzz coverage, and 32 Grade-A tests. Since then, Chrome (M125, 2025) and Safari (17, 2023) have both turned on H.265 send/receive in WebRTC by default. Chrome's offer typically bundles VPS + SPS + PPS + IDR as an Aggregation Packet (RFC 7798 §4.4.2) — so any NanoRTC receiver that claims H.265 support must at least parse APs correctly on the wire.

The H.264 module architecture is already codec-pluggable: per-track `codec` field, per-track depacketizer union, codec-specific rtpmap/fmtp handling in the SDP module, and a callback-based packetizer API. Adding H.265 is a transplant of that pattern, not a refactor.

**User decisions recorded during planning (2026-04-13):**
1. **Direction:** Bidirectional send + receive (not recvonly-first). Requires sprop-vps/sps/pps emission → requires a new base64 helper and a new public API for the caller to provide VPS/SPS/PPS out-of-band before SDP generation.
2. **Aggregation Packet:** Full send + receive (not receive-only). The send path uses a greedy Single / AP / FU packer (RFC 7798 §4.4.1/§4.4.2/§4.4.3).
3. **Feature flag:** `NANORTC_FEATURE_H265` as a sub-feature of `NANORTC_FEATURE_VIDEO`, defaulting **ON** when VIDEO is on. ESP32/flash-sensitive builds can opt out to save ~11 KB of code.

**Authority discipline (RFC iron rule):**
All behavior decisions trace to RFC 7798 sections cited in source comments. Test vectors are generated independently from RFC 7798 bit layouts, from x265 raw NAL output, or from Chrome Wireshark captures — **no byte sequences copied from libdatachannel, libwebrtc, or str0m source/test trees.**

## Scope (first pass = all 3 PRs combined)

### In scope

- **RFC 7798 §4.4.1** Single NAL Unit Packet — send + receive
- **RFC 7798 §4.4.2** Aggregation Packet — send + receive. Greedy packer combines consecutive small NALs into one AP; receiver returns the first inner NAL (matches H.264 STAP-A precedent at `src/nano_h264.c`).
- **RFC 7798 §4.4.3** Fragmentation Unit — send + receive. S/E/FuType bits, PayloadHdr carries original LayerId/TID, original NAL header reconstructed at receiver. DON field never emitted (sprop-max-don-diff=0 assumption).
- **RFC 7798 §4.1** Single Stream (SRST) transmission mode only. Strict rejection of `tx-mode` ≠ SRST in remote fmtp.
- **RFC 7798 §7.1** SDP parameters: `profile-id=1`, `tier-flag=0`, `level-id=93`, `tx-mode=SRST` emitted by default. Parser rejects `sprop-max-don-diff>0` (§6.1) — first pass does not implement DON reordering.
- **sprop-vps / sprop-sps / sprop-pps** emission for send-to-browser scenarios, via new API `nanortc_video_set_h265_parameter_sets(rtc, mid, vps, vps_len, sps, sps_len, pps, pps_len)` called between `nanortc_add_video_track()` and `nanortc_create_offer()`.
- **IRAP keyframe detection** — NAL types 16..23 per RFC 7798 §1.1.4 / H.265 §7.4.2.2, inspected across Single NAL, AP (any inner NAL IRAP → keyframe), and FU start fragments.

### Out of scope (first pass)

- **RFC 7798 §4.4.4** PACI Packet — parser logs and drops; no sender support. Rationale: PACI is MAY per spec and no browser requires it.
- **RFC 7798 §4.1 item 2/3** Multi-Session (MSST/MSMT) transmission modes — rejected on receive (fmtp `tx-mode` ≠ SRST).
- **RFC 7798 §6.1** DON-based decoding-order reshuffling — first-pass receiver always delivers NALs in RTP receive order.

## Architecture summary

### Per-track state

`src/nano_media.h` `track.video` evolves from:

```c
struct { nano_h264_depkt_t h264_depkt; } video;
```

to:

```c
struct { union { nano_h264_depkt_t h264; nano_h265_depkt_t h265; } depkt; } video;
```

so that the per-track RAM cost is `max(sizeof(nano_h264_depkt_t), sizeof(nano_h265_depkt_t))` — `nano_h265_depkt_t` is 2 bytes larger (it stores the reconstructed 2-byte NAL header instead of H.264's 1 byte).

### Public API surface (PR-2)

- `NANORTC_CODEC_H265` appended to `nanortc_codec_t` enum.
- Reuses `nanortc_add_video_track(rtc, direction, NANORTC_CODEC_H265)`.
- New API: `nanortc_video_set_h265_parameter_sets()` — caller provides raw VPS/SPS/PPS NAL bytes (2-byte NAL header + RBSP), the library base64-encodes them and stores the pre-formatted fmtp fragment in the SDP m-line.
- `nanortc_send_video()` unchanged — accepts Annex-B access unit. Internally scans NAL units and dispatches to `h265_packetize_au()` when the track codec is H.265, `h264_packetize()` when H.264.

### SDP emission (PR-2)

```
m=video 9 UDP/TLS/RTP/SAVPF 98
c=IN IP4 0.0.0.0
a=mid:<mid>
a=<direction>
a=ice-ufrag:... / a=ice-pwd:... / a=fingerprint:... / a=setup:...
a=rtcp-mux
a=rtpmap:98 H265/90000
a=fmtp:98 profile-id=1;tier-flag=0;level-id=93;tx-mode=SRST[;sprop-vps=...;sprop-sps=...;sprop-pps=...]
a=rtcp-fb:98 nack pli
```

The sprop-* fragment is only emitted when the caller has invoked `nanortc_video_set_h265_parameter_sets()`. Omitting it is RFC-legal per §7.1 ("sprop-* parameters are OPTIONAL") and works for in-band parameter-set streams.

### Default payload type

H.265 defaults to **PT = 98**, disjoint from H.264's PT = 96, so a future same-m-line dual-codec offer can carry both without renumbering. Configurable via `NANORTC_VIDEO_H265_DEFAULT_PT`.

## Sub-PRs

| PR | Topic | Primary files | Risk | Expected win |
|---|---|---|---|---|
| **PR-1** | `nano_h265` / `nano_base64` / `nano_annex_b` modules + unit tests + fuzz | `src/nano_h265.{c,h}`, `src/nano_base64.{c,h}`, `src/nano_annex_b.{c,h}`, `tests/test_h265.c`, `tests/test_base64.c`, `tests/fuzz/fuzz_h265.c`, `tests/fuzz/corpus/h265/`, `include/nanortc_config.h`, CMake wiring | **Low** (purely additive; H.264 path unchanged via `#define` alias) | RFC 7798 packetizer / depacketizer / keyframe detector ready for wiring. Zero behavior change to existing code. |
| **PR-2** | SDP + `nano_rtc` wiring + parameter-sets API + e2e loopback | `include/nanortc.h`, `src/nano_sdp.{c,h}`, `src/nano_rtc.c`, `src/nano_media.h`, `tests/test_sdp.c`, `tests/test_e2e.c` | Medium (touches SDP parser, RTC send/receive dispatch, per-track union layout) | End-to-end H.265 session negotiation + loopback send/receive between two NanoRTC instances. |
| **PR-3** | libdatachannel interop + browser example + doc finalization | `tests/interop/test_interop_video.c`, `examples/linux_browser_h265.{c,html}`, `docs/**/*.md`, `docs/exec-plans/active/phase3-5-h265.md` → `completed/` | Medium (requires manual browser verification) | Browser interop verified + docs finalized + module promoted to Grade A. |

---

## PR-1 — `nano_h265` / `nano_base64` / `nano_annex_b` modules + tests + fuzz

**Status:** Ready for review.

### What lands

**New files:**
- `src/nano_h265.{c,h}` (~700 LOC) — `h265_packetize`, `h265_packetize_au` (greedy AP/Single/FU), `h265_depkt_init`, `h265_depkt_push` (Single/AP/FU/PACI), `h265_is_keyframe` (Single/AP/FU with IRAP check). Every non-obvious branch cites a RFC 7798 section.
- `src/nano_annex_b.{c,h}` — shared Annex-B start-code scanner. Physically migrated from `nano_h264.c:302-354` into its own TU. `src/nano_h264.h` now provides `#define h264_annex_b_find_nal nano_annex_b_find_nal` so existing H.264 call sites and tests compile unchanged.
- `src/nano_base64.{c,h}` — RFC 4648 §4 standard-alphabet encoder, single function `nano_base64_encode`. No decoder — not needed by H.265 SDP path.
- `tests/test_h265.c` — **51 Unity tests** covering Single NAL / FU / AP packetizer, FU / AP / PACI / unknown-type depacketizer, IRAP keyframe detection across Single / AP / FU. All byte-level vectors hand-crafted from RFC 7798 §4.4 bit layouts.
- `tests/test_base64.c` — 12 tests: the 7 canonical RFC 4648 §10 vectors + 5 implementation-specific edge cases (alphabet coverage, buffer overflow, NUL termination, empty-input-with-dst, encoded-size helper).
- `tests/fuzz/fuzz_h265.c` — harness targeting `h265_depkt_push`, `h265_is_keyframe`, and `nano_annex_b_find_nal`. Structure mirrors `fuzz_h264.c`.
- `tests/fuzz/corpus/h265/{single_nal_idr,fu_start_idr,fu_mid,fu_end,ap_vps_sps_pps}.bin` — 5 hand-constructed seed files.

**Modified files:**
- `include/nanortc_config.h` — new macros: `NANORTC_FEATURE_H265` (default = `NANORTC_FEATURE_VIDEO`), `NANORTC_VIDEO_H265_DEFAULT_PT=98`, `NANORTC_H265_SPROP_FMTP_SIZE=512`, `NANORTC_MAX_NALS_PER_AU=16`. Validation stanza errors out if H265 is on without VIDEO.
- `CMakeLists.txt` — new `NANORTC_FEATURE_H265` CMake option (defaults to `NANORTC_FEATURE_VIDEO`), validation check, source list entries under both host and ESP-IDF branches.
- `src/nano_h264.{c,h}` — `h264_annex_b_find_nal` function body removed from `.c`; `.h` includes `nano_annex_b.h` and defines the `#define` alias. Zero behavior change.
- `tests/CMakeLists.txt` — new `H265_TESTS` category (`test_h265`, `test_base64`) gated on `NANORTC_FEATURE_H265`.
- `tests/fuzz/CMakeLists.txt` — new `FUZZ_H265_TARGETS` bucket.
- `scripts/ci-check.sh` — added `h265_` to the allowed symbol prefix regex.

**Not touched in PR-1:** `include/nanortc.h` (no enum change yet), `src/nano_sdp.{c,h}` (no rtpmap/fmtp for H.265 yet), `src/nano_rtc.c` (no send/receive dispatch), `src/nano_media.h` (union layout unchanged). Because nothing new is called from existing code, PR-1 is bisect-safe.

### Verification (PR-1 actually completed)

- **Unit tests:** 51 H.265 tests + 12 base64 tests, all pass on the primary MEDIA+openssl build.
- **Regression:** all 32 H.264 tests still pass (annex-b moved via `#define` alias).
- **Build matrix:** `./scripts/ci-check.sh` passes all 36 checks: 6 feature profiles × 2 crypto backends = 12 build+test combinations, symbol prefix check, AddressSanitizer MEDIA build+test, libdatachannel interop, clang-format clean.
- **H.265 off:** independent build with `-DNANORTC_FEATURE_H265=OFF` builds and passes its 18 tests (`test_h265` and `test_base64` correctly excluded).
- **Code size budget:** `nano_h265.c` (~700 LOC) + `nano_base64.c` (~80 LOC) + `nano_annex_b.c` (~60 LOC, migrated). Net code growth with `NANORTC_FEATURE_H265=ON` is ~11 KB text on armv7-m `-Os`.
- **Fuzz:** harness compiles and links (verified via gcc standalone), LibFuzzer execution deferred to the next CI run (clang+libFuzzer required).

### Acceptance criteria

- [x] RFC 7798 §4.4.1 / §4.4.2 / §4.4.3 send-side implementations present and unit-tested
- [x] RFC 7798 §4.4.1 / §4.4.2 / §4.4.3 receive-side implementations present and unit-tested
- [x] RFC 7798 §1.1.4 NAL header decoding helpers
- [x] RFC 7798 §1.1.4 IRAP keyframe detection across Single / AP / FU
- [x] RFC 4648 §10 base64 vectors
- [x] No source-level dependence on reference implementation test fixtures
- [x] 6-profile × 2-crypto build matrix passes (`scripts/ci-check.sh`)
- [x] AddressSanitizer clean on the MEDIA profile
- [x] clang-format clean
- [x] Independent `NANORTC_FEATURE_H265=OFF` build still works

**Quality grade at end of PR-1:** `nano_h265.c` = **B** (functional, RFC-compliant, 51 unit tests with hand-crafted vectors). Promotes to **A** after PR-3 browser verification + ≥50M fuzz execs.

---

## PR-2 — SDP + `nano_rtc` wiring + parameter-sets API + e2e

**Status:** Pending.

### What to land

- **Public API** (`include/nanortc.h`):
  - `NANORTC_CODEC_H265` appended to `nanortc_codec_t` enum.
  - New `nanortc_video_set_h265_parameter_sets()` declaration, gated on `NANORTC_FEATURE_H265`.
- **SDP m-line state** (`src/nano_sdp.h`): add `video_h265_rtpmap_pt` sibling to `video_h264_rtpmap_pt`, and `h265_sprop_fmtp[]` + length for the pre-formatted sprop-* fragment.
- **SDP parser** (`src/nano_sdp.c`):
  - `parse_rtpmap` branch for `H265/90000`.
  - fmtp parser branch accepting `profile-id`, `tier-flag`, `level-id`, rejecting `tx-mode ≠ SRST` and `sprop-max-don-diff > 0`.
- **SDP builder** (`src/nano_sdp.c` `sdp_append_video_mline`): dispatch on `ml->codec` to emit either H.264 or H.265 rtpmap/fmtp/rtcp-fb block.
- **PT assignment** (`src/nano_rtc.c` `nanortc_add_track`): H.264 → 96, H.265 → `NANORTC_VIDEO_H265_DEFAULT_PT` (98).
- **Negotiation** (`src/nano_rtc.c` `rtc_apply_negotiated_media`): select H.264 or H.265 rtpmap PT based on local track's codec.
- **Send path** (`src/nano_rtc.c` `rtc_send_video` / `nanortc_send_video`): scan Annex-B access unit to build `h265_nal_ref_t` stack array (≤ `NANORTC_MAX_NALS_PER_AU`), call `h265_packetize_au()` when codec is H.265, else `h264_packetize()` per-NAL.
- **Receive path** (`src/nano_rtc.c` RTP dispatch): call `h265_depkt_push()` or `h264_depkt_push()` based on `m->codec`.
- **`nanortc_video_set_h265_parameter_sets()` implementation**: validate mid + codec, base64-encode VPS/SPS/PPS via `nano_base64_encode`, format `sprop-vps=..;sprop-sps=..;sprop-pps=..` into the mline scratch buffer.
- **Per-track union** (`src/nano_media.h`): convert `track.video.h264_depkt` to an inner union of h264/h265 depacketizers. Every access site in `nano_rtc.c` updated.
- **Tests** (`tests/test_sdp.c`): 4 new SDP tests — basic H.265 offer parse, reject non-SRST tx-mode, reject non-zero sprop-max-don-diff, generate answer with sprop-*.
- **E2E** (`tests/test_e2e.c` or new `tests/test_e2e_h265.c`): loopback between two `nanortc_t` instances negotiating H.265, sending an Annex-B frame, receiving via `NANORTC_EV_MEDIA_DATA`, comparing bytes.

### Acceptance criteria

- [ ] `nanortc_add_video_track(rtc, dir, NANORTC_CODEC_H265)` returns a valid MID
- [ ] `nanortc_video_set_h265_parameter_sets()` emits correctly base64-encoded sprop-* in the generated SDP
- [ ] Two-nanortc loopback: `set_parameter_sets` → `create_offer` → `accept_answer` → `send_video(Annex-B AU)` → `EV_MEDIA_DATA` with byte-identical payload
- [ ] H.264 existing test suite still passes unchanged
- [ ] 6-profile × 2-crypto × `NANORTC_FEATURE_H265={ON,OFF}` build matrix passes

**Quality grade at end of PR-2:** `nano_h265.c` = **B** (all wiring in place, end-to-end loopback verified, fuzz execs ramped up).

---

## PR-3 — libdatachannel interop + browser example + doc finalization

**Status:** Pending.

### What to land

- **libdatachannel interop** (`tests/interop/test_interop_video.c`): new H.265 case mirroring the existing H.264 case. Requires libdatachannel ≥ 0.22 with H.265 support.
- **Browser example** (`examples/linux_browser_h265.{c,html}`): Linux host app that negotiates H.265 recv + send with Chrome M130+, dumps received Annex-B to disk for playback verification with `ffmpeg`.
- **Doc finalization:**
  - `docs/design-docs/nanortc-design-draft.md` — video chapter gains an H.265 paragraph citing RFC 7798 §4.4 and §7.1.
  - `docs/engineering/memory-profiles.md` — memory table updated with H.265 sprop buffer cost.
  - `docs/QUALITY_SCORE.md` — `nano_h265.c` promoted to **A**.
  - This exec plan moved from `active/` to `completed/`.
- **Tech debt closure:** nothing — no TDs expected to be opened by H.265.

### Acceptance criteria

- [ ] libdatachannel ↔ NanoRTC H.265 interop test passes in CI
- [ ] Manual: Chrome sends H.265 to NanoRTC, `RTCPeerConnection.getStats()` shows `framesReceived > 0` for 10 s continuously
- [ ] Manual: NanoRTC sends H.265 to Chrome (with `set_parameter_sets`), page displays decoded frames
- [ ] Fuzz `fuzz_h265` accumulates ≥ 50M executions clean (parity with `fuzz_h264` baseline)
- [ ] `nano_h265.c` line coverage ≥ 80% (per `scripts/coverage.sh --threshold 80`)

**Quality grade at end of PR-3:** `nano_h265.c` = **A**.

---

## Risks

1. **Chrome H.265 SDP format drift.** Between M119 and M130 the Chrome WebRTC H.265 offer format has changed at least three times. PR-3 must record the exact Chrome version used for the capture corpus in `tests/fuzz/corpus/h265/README.md`.
2. **sprop-vps/sps/pps size.** JCT-VC high-resolution streams can have SPS NAL units > 100 bytes; base64 expansion brings that to ~140 chars. `NANORTC_H265_SPROP_FMTP_SIZE=512` covers VPS(32) + SPS(128) + PPS(32) base64 + keys + separators ≈ 304 chars. Users can override to 1024 for extreme streams.
3. **Non-zero `sprop-max-don-diff`.** First-pass parser rejects remote PTs that declare it, because the first-pass depacketizer does not reorder by DON. This is spec-compliant per RFC 7798 §6.1 as a receiver choice, but means we cannot consume streams from peers that insist on DON.
4. **AP packing at tight MTU.** Greedy packer must not loop or overrun when given a pathological MTU/NAL-size combination. Covered by `test_h265_pack_ap_then_single` and fuzz.
5. **Per-track union layout change.** PR-2's switch from plain `h264_depkt` to `union {h264, h265} depkt` breaks any code that reads `track.video.h264_depkt` directly. A repo-wide grep confirmed only `src/nano_rtc.c` accesses this field, so the change is contained.

## Decision log

- **2026-04-13** — Planning. User selected: bidirectional H.265 (not recvonly-first), full AP send+receive (not receive-only), `NANORTC_FEATURE_H265` sub-flag default ON. PT = 98 (disjoint from H.264 = 96). DON handling: reject non-zero `sprop-max-don-diff` rather than silently skip bytes. Annex-B scanner shared via alias, not duplicated.
- **2026-04-13** — PR-1 complete: module + tests + fuzz harness + CI check pass. PR-2 and PR-3 pending.
- **2026-04-16** — Browser interop hardening (post-PR-2). Four bugs surfaced once
  `uipcat-camera-rk3588` started driving real Chrome / Safari viewers at
  1920×1080 H.265:

  1. **`sdp_parse()` wiped local m-line state.** The `memset(ml, 0, sizeof(*ml))`
     on each `m=audio` / `m=video` line cleared `ml->codec` and
     `ml->h265_sprop_fmtp` that the caller had set via `add_video_track()` and
     `set_h265_parameter_sets()`. The answer fell back to H.264 / omitted
     sprop-*. Fix: snapshot the preserved fields at parse entry, restore them
     by index when re-creating each m=line.
  2. **H.265 rtpmap without fmtp left `ml->pt` at the local default.** Safari
     advertises H.265 with only `a=rtpmap:N H265/90000` and no companion
     `a=fmtp:N ...`. The fmtp-driven PT selection never triggered, so the
     answerer echoed PT=98 — but PT 98 on Safari's side maps to H.264, so
     Safari discarded every RTP packet. Fix: set `ml->pt` inside
     `parse_rtpmap()` whenever an H.265 rtpmap is seen and the local track is
     H.265. The H.265 fmtp branch still overrides when present.
  3. **H.264 preferred-profile match hijacked H.265 tracks.** Chrome's offer
     carries both H.264 `profile-level-id=42e01f` and an H.265 block. The
     existing `is_valid_h264 && has_preferred_profile` selector would set
     `ml->pt` to the H.264 PT even when the local track was explicitly H.265.
     Fix: gate that branch with `ml->codec != NANORTC_CODEC_H265`.
  4. **`level-id` hardcoded to 93 broke Safari.** The SDP emitter wrote
     `level-id=93` (Level 3.1) even for 1080p30 streams (actually Level 4.0,
     `level_idc=120`). Safari's HEVC decoder drops frames when SDP level-id
     understates the stream level. Fix: `set_h265_parameter_sets()` now parses
     the VPS `profile_tier_level()` — stripping H.265 §7.4.1.1 emulation-
     prevention 0x03 bytes along the way — and stores `h265_profile_id` /
     `h265_tier_flag` / `h265_level_id` on the m-line. The emitter uses those
     values when set, falling back to the compile-time defaults otherwise.

  Regression coverage: `tests/test_sdp.c` gained
  `test_sdp_h265_rtpmap_without_fmtp_picks_remote_pt`,
  `test_sdp_h265_local_track_not_hijacked_by_h264_fmtp`, and
  `test_sdp_parse_preserves_local_h265_state`.
