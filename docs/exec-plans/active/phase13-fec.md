# Phase 13: Forward Error Correction (FEC) — design + plan

**Status:** PR-1 (ULPFEC codec) + **PR-3 (send-side group generation)** + **PR-4 (receive-side recovery + re-inject)** **implemented**, tested (codec structural/roundtrip + `fuzz_fec` + a loopback E2E `test_e2e_video_fec_recovers_drop`: drop one media packet, recover it from FEC with **zero retransmit**) + CI-green in the `REORDER+NACK_RX+FEC` combo (host build, ASan-clean, 28/28). **PR-2 (SDP red/ulpfec negotiation) + PR-5 (browser interop) pending** — see "Browser-interop gap" below. The shipped send/recv scheme is **separate-SSRC ULPFEC** (media SSRC + 1, dynamic PT `NANORTC_VIDEO_FEC_PT`), which is the nanortc↔nanortc loopback wire; RED-wrapping for Chrome is PR-2.
**Estimated effort:** 3–4 agent sessions (codec → SDP/RED → send/recv → interop), sequenced.
**Goal:** Add proactive, zero-RTT loss recovery for the cases NACK can't serve — high one-way-delay or high-loss links where a retransmit arrives too late for the playout deadline — without regressing the embedded memory/CPU budget.

## Why this is the *last* loss-recovery primitive, not the first

The recovery loop already shipped covers the common embedded cases:

| Primitive (shipped) | Recovers | Latency | Bandwidth cost |
|---|---|---|---|
| Pacing (Phase 11) | prevents self-inflicted burst loss | — | none |
| Reorder buffer (Phase 12 PR-2) | reordering (not loss) | ≤ cap | none |
| **NACK** (Phase 12 PR-3) | a lost packet, on demand | ~1 RTT | only on loss (cheap) |
| auto-PLI (Phase 12 PR-1) | undecodable stream | ~1 RTT + IDR | a keyframe |

**FEC is the only one that costs bandwidth even with zero loss** (constant
redundancy). On a bandwidth-constrained embedded *uplink* (the camera flagship),
that overhead competes directly with video quality, so FEC is **not** a default —
it earns its place only when (a) the link RTT exceeds the jitter/playout budget
(so NACK is too late) or (b) loss is high enough that NACK retransmits are
themselves lost. Hence: **opt-in, default off**, like reorder/NACK.

## Scheme decision (must be made before coding)

| Scheme | Pros | Cons (embedded) | Interop |
|---|---|---|---|
| **RED + ULPFEC** (RFC 2198 + 5109) | Widely supported; ULPFEC carried in RED; simple XOR | Per-packet FEC header bit-recovery is fiddly; RED wrapper adds framing | Chrome (legacy), Firefox |
| **FlexFEC** (RFC 8627) | Modern, flexible masks, separate stream | More complex masks; newer | Chrome (recent) behind flags |
| **Internal XOR FEC** (non-standard) | Simplest to implement + test | **No browser interop** | nanortc↔nanortc only |

**Recommendation:** target **RED + ULPFEC level-0** (single protection level,
16-bit contiguous mask). It is the most broadly interoperable and the XOR core is
small; FlexFEC can follow. An internal-only scheme is rejected — it fails the
project's browser-interop bar.

**Open question for the human:** confirm RED+ULPFEC (vs FlexFEC) and the target
overhead (e.g. 1 FEC packet per K media packets ⇒ 1/K overhead; K configurable).

## Wire format (RFC 5109 §7.3/§7.4, level 0)

A FEC packet protects a contiguous group of K media packets:
- **FEC header (10 B):** recovery XOR of the media RTP headers' {P,X,CC,M,PT},
  `SN base` (lowest protected SN), `TS recovery` (XOR of timestamps),
  `length recovery` (XOR of each media packet's post-fixed-header octet count).
- **FEC level-0 header (4 B):** `protection length` + 16-bit `mask` (bit i ⇒
  SN base+i protected).
- **FEC payload:** XOR of the protected media payloads (zero-padded to the
  longest).
- Carried inside a **RED** block (RFC 2198) on the FEC payload type.

Decode: on exactly one missing packet in a group, XOR the FEC packet with the
received members to reconstruct the missing RTP header fields + payload. (Two+
missing in a group ⇒ unrecoverable by level-0; falls back to NACK/PLI.)

## Sub-PRs (sequenced, each independently testable)

| PR | Scope | Validatable without a browser? |
|----|-------|--------------------------------|
| **PR-1** ✅ | `nano_fec.{c,h}` codec: ULPFEC encode (K media → FEC pkt) + decode (recover 1). `test_fec.c` (structural RFC-field assertions + byte-exact roundtrip across group sizes / variable lengths / wraparound / unrecoverable cases) + `fuzz_fec` (ASan/UBSan clean). **default off.** | **Yes** — done. |
| **PR-2** | SDP: emit/parse `a=rtpmap` red/ulpfec + `a=fmtp` apt; payload-type plumbing; RED-wrap on the wire. | Yes — SDP unit + libdatachannel offer/answer. **Pending.** |
| **PR-3** ✅ | Send path: capture plaintext per K (after `rtp_pack`, before SRTP), `fec_encode`, emit one FEC RTP packet on the FEC SSRC. `rtc_fec_capture()` in `nano_rtc_media.c`. | **Yes** — loopback e2e done. |
| **PR-4** ✅ | Receive path: PT-intercept the FEC stream, buffer recent media (ring), recover the one missing member, re-inject into reorder/deliver. `rtc_fec_on_recv()` / `rtc_fec_buffer_media()`. | **Yes** — loopback e2e (`test_e2e_video_fec_recovers_drop`). |
| **PR-5** | libdatachannel interop (does it consume our ULPFEC? do we recover from its FEC?) + **browser smoke** (human). | **No** — needs interop harness / browser. **Pending.** |

## Implementation notes (PR-3/4 as shipped) — read before review

The shipped design diverges from the original plan in three deliberate ways;
each is a memory/complexity trade recorded here so a reviewer can challenge it:

1. **Separate FEC SSRC, not RED-wrapped (yet).** The FEC packet is a normal RTP
   packet on `media SSRC + 1`, PT `NANORTC_VIDEO_FEC_PT`, carrying the
   `fec_encode` body as its payload. The receiver PT-intercepts it before the
   SSRC→MID lookup. This is correct nanortc↔nanortc but **not** what Chrome
   expects (Chrome wants ULPFEC inside a RED block on the media PT, per the SDP
   `a=rtpmap … red/ulpfec`). **RED-wrapping + SDP negotiation = PR-2**, the
   browser-interop gate. The codec and recovery loop are scheme-independent, so
   PR-2 is framing only.
2. **FEC is NOT paced; it is enqueued out-of-band with a single-buffer reuse
   guard.** Pacing the FEC would require either a pkt_ring slot (it doesn't fit —
   a full-MTU FEC packet is ~`MTU+36` > `MEDIA_BUF_SIZE`) or a parallel paced
   queue. Instead `fec_tx_buf` is a single buffer; a new group's FEC is emitted
   only once the previous FEC output has been dequeued (`out_head` reached
   `fec_tx_free_at`). Consequence — **limitation L1**: a bursty frame fragmenting
   into > K packets emits FEC for only its *first* group per poll cycle; the rest
   fall back to reorder/NACK/auto-PLI. FEC is supplementary, so this is
   acceptable, but full multi-group coverage (a small FEC tx ring) is a
   follow-up. The guard guarantees **no aliasing** — a live output is never
   overwritten, so corrupt FEC never reaches the wire.
3. **Receive recovery is wire-order-independent.** Because the FEC is enqueued
   out-of-band, it can reach the receiver *before* its group's media. The
   receiver buffers one pending FEC and retries recovery as each media packet is
   admitted, so ordering never matters. **Limitation L3**: only one FEC is held
   pending (one group's reordering); a real network delivers FEC after its media
   so this is rarely exercised. **Limitation L2**: FEC state is rtc-level
   (single video track); multi-video-track FEC needs per-track state.

4. **FEC requires the reorder buffer** (`#error` if `VIDEO_FEC=1 &&
   VIDEO_REORDER=0`). A recovered packet arrives "late" and must be slotted into
   sequence to be useful; the reorder buffer is also the only safe re-injection
   path — it copies the NAL into its own slot and releases one-per-poll, so the
   recovered packet and the live packet that triggered recovery never alias the
   single per-track depacketizer buffer in one `handle_input`. (Adversarial
   review caught the non-reorder immediate-deliver path doing exactly that — two
   `EV_MEDIA_DATA` aliasing one buffer; requiring REORDER eliminates the broken
   config at compile time.)

### Adversarial-review fixes folded in (6 confirmed findings)

A 3-lens × per-finding-refutation review of the integration ran post-implementation:
- **HIGH** (×2, same root) — non-reorder re-inject aliasing → **fixed** by the
  requires-REORDER `#error` above.
- **MEDIUM** — stale FEC receive ring/pending survived a mid-session SSRC change
  → **fixed** (reset `fec_rx_n` / `fec_rx_pending_len` in the SSRC-change block).
- **MEDIUM** — the FEC SSRC consumes an SRTP per-SSRC slot per direction →
  **fixed** (`NANORTC_MAX_SSRC_MAP` gains +2 under FEC, with a validation `#error`).
- **MEDIUM/LOW** — PT-only FEC interception could hijack a media stream sharing
  PT 116 → **fixed** (compile-time PT-disjointness guards + the interception is
  scoped to *unregistered* SSRCs at runtime).
- One finding (FEC dropped when a media packet exceeds `MEDIA_BUF`) was **refuted**
  (the length guard handles it). The residual NACK-coordination gap is tracked in
  TD-026.

RTP header extensions (TWCC) are handled correctly — XOR recovery is byte-exact,
so the recovered packet's X bit, extension, and payload are reconstructed
verbatim (no fixed-12-byte-header assumption beyond CC=0, which nanortc always
satisfies).

### Browser-interop gap (PR-2 + PR-5, the remaining "done" bar)

The recovery loop is validated nanortc↔nanortc only. Chrome interop requires
RED-framed ULPFEC + the SDP `a=rtpmap`/`a=fmtp` negotiation (PR-2) and then a
browser smoke (PR-5, human/hardware). Until then FEC must stay **off** for
browser-facing deployments; it is functional for nanortc↔nanortc links today.

### Adaptive FEC group size (implemented; panel-selected follow-up)

The biggest embedded-bandwidth win on the scarce camera uplink: the group size K
(overhead 1/K) **tracks the smoothed TWCC loss fraction** instead of a fixed K.

- `nano_bwe.c` retains a `smoothed_loss_q8` EMA of the TWCC loss samples it
  already receives (RFC 8888), exposed via `bwe_get_loss_q8()`.
- `rtc_fec_effective_k()`: loss `< NANORTC_FEC_LOSS_OFF_Q8` (~2%) → **K=0, no FEC
  at all** (zero overhead on a clean link); loss `≥ NANORTC_FEC_LOSS_HIGH_Q8`
  (~10%) → `NANORTC_FEC_MIN_GROUP` (most protection); else the full group. K is
  clamped to `NANORTC_FEC_GROUP_SIZE` (the fixed ring size) so it never overflows.
- **Sender-only / wire-compatible**: K only varies *down* from the configured max
  so the fixed `fec_tx_grp`/`fec_rx_med` rings are never exceeded, and the
  receiver is K-agnostic (the RFC 5109 level-0 mask carries the group size) — no
  receive-side change. The captured group is always K *consecutive* sequence
  numbers (span < 16), so the level-0 mask invariant holds.
- Verified by `test_e2e_video_fec_adaptive_k_tracks_loss` (clean → 0 FEC,
  moderate → 1 FEC per K frames, high → K/MIN_GROUP FEC). Config: `NANORTC_FEC_
  ADAPTIVE` (default 1), `NANORTC_FEC_LOSS_OFF_Q8`, `NANORTC_FEC_LOSS_HIGH_Q8`,
  `NANORTC_FEC_MIN_GROUP`, all `#error`-validated.

## Embedded budget (must hold)

- **Memory (as shipped, opt-in):** send group `fec_tx_grp` (K × MEDIA_BUF ≈
  10 KB) + `fec_tx_buf` (FEC_BUF ≈ 1.3 KB) + receive ring `fec_rx_med`
  (K × MEDIA_BUF ≈ 10 KB) + `fec_rx_recov`/`fec_rx_pending` (≈ 2.5 KB) ≈ **~24 KB
  total at K=8**, all under `NANORTC_FEATURE_VIDEO_FEC`. State is rtc-level
  (single-video-track scope), so a send-only camera still allocates the receive
  ring — a per-direction split is a future trim if needed.
- **CPU:** XOR is cheap (memxor over MTU per FEC packet); no Galois-field math at
  level 0.
- **Config:** `NANORTC_FEATURE_VIDEO_FEC` (default 0), `NANORTC_FEC_GROUP_SIZE`
  (K, default 8), plus the SDP payload types. Compile-time `#error` validation.

## Interaction with shipped work (designed, not incidental)

- **Pacer (as shipped):** FEC is enqueued out-of-band, *not* through the pacer
  (it doesn't fit a pkt_ring slot — see note 2). One ~MTU FEC packet per K media
  is a small, infrequent burst; pacing it is a future refinement, not a
  correctness need. The single-buffer reuse guard bounds in-flight FEC to one per
  poll cycle.
- **Reorder buffer (as shipped):** a FEC-recovered packet is `reorder_push`'d at
  its real SN, so the reorder buffer slots it in order; recovery runs as media is
  admitted (before the reorder skip cap on a healthy link).
- **NACK: now coordinated** (Phase 13 follow-up). The receiver records each FEC
  packet's protected SN window (`fec_prot_base`/`fec_prot_mask`, set in
  `rtc_fec_on_recv` from the RFC 5109 level-0 SN base + mask) and the NACK
  gap-detect skips any SN that window covers (`rtc_fec_protects()`), emitting a
  NACK only for the unprotected remainder. Because nanortc sends the FEC packet
  *ahead* of its paced media, the protected window is known before the gap is
  observed — so no deferral timer is needed. Policy is **eager** ("FEC owns its
  groups"): a 2+-loss group's surplus losses fall to reorder-skip → auto-PLI
  rather than NACK (level-0 FEC can't recover them anyway, and on a bursty link a
  keyframe beats a likely-also-lost retransmit). A *precise* "NACK only the
  FEC-unrecovered" variant (deferral window) is a later refinement. Verified:
  `test_e2e_video_fec_recovers_drop` asserts `stats_nack_sent==0` +
  `stats_nack_suppressed_fec>=1`; `test_e2e_video_nack_recovers_drop` (no FEC
  group) confirms NACK still fires for non-FEC-protected losses.
- **TD-023:** the FEC stream rides a *separate* SSRC and does **not** enter the
  media pkt_ring, so it does not touch the TD-023 aliasing window — the FEC tx
  path uses its own single-buffer guard instead.

## Decision required before PR-1

1. Scheme: **RED+ULPFEC** (recommended) vs FlexFEC.
2. Default K (overhead = 1/K) and whether FEC is send-only / recv-only / both by
   default when enabled.
3. Sequencing vs landing Phase 11–12 first (recommended: **land + browser-smoke
   11–12, then start PR-1**) — building FEC atop an unreviewed 1.5k-line branch
   compounds review risk for no functional gain (FEC PR-1 is independent).

This doc is the groundwork so PR-1 can start cleanly once the scheme + sequencing
are confirmed.
