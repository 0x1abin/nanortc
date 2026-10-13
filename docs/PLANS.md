# Execution Plans Index

Plans are first-class artifacts in NanoRTC. They are versioned, tracked, and co-located with the code.

## Effort Model

NanoRTC is built by AI coding agents. Estimates use **agent sessions** (one focused run, typically 2-6 hours of agent work) rather than human-weeks. The primary bottleneck is human review and browser-level verification, not coding speed.

## Current release and active plans

**0.2.0 implementation and release validation — completed 2026-10-13:**
[API, authentication, bounded I/O, ICE restart, statistics and BWE](engineering/nanortc-02.md).
Full CI passes 54/54 checks across seven profiles and both crypto providers.
All-advanced ASan/UBSan builds pass 43/43 suites on each provider; 12 fuzz harnesses completed
14 runs and 96,285,341 executions without errors. Both-role session restart,
50/100/200 ms weak-network simulations, four Chrome sessions, four independent
peer interop suites, four ESP-IDF examples and seven P4 profiles are verified
within the linked record's scope. Performance and exact memory values are
recorded there. This release breaks API compatibility; hardware runtime,
LeakSanitizer and browser encoder-adaptation timing are not claimed. Earlier
completed-plan browser/fuzz totals below are dated historical results.


Issue #81 implementation and validation: [bidirectional ICE](engineering/issue-81-ice.md). Example portability is delivered separately.

| Plan | Phase | Status | Effort | Target |
|------|-------|--------|--------|--------|
| [Phase 2: Audio](exec-plans/active/phase2-audio.md) | 2 | **Active** — Pending human verification of bidirectional audio + ESP32 intercom | 1 session | Bidirectional audio with browser |
| [Phase 8: Continued Optimization & Stability](exec-plans/active/phase8-continued-optimization.md) | 8 | **Active** — PR-2 (SCTP failure event propagation) landed 2026-04-25, PR-3 (video pkt_ring decoupling) landed 2026-04-23, PR-4 (H.264 zero-copy iterator) landed 2026-04-25, PR-5 (ICE CONTROLLING fix) landed 2026-04-13. PR-1 pending; P2 series on demand. | 4–6 sessions | IoT memory profile, stability propagation, video pkt_ring decoupling, H.264 zero-copy, ICE CONTROLLING per-pair transaction fix |
| [Phase 9: BWE Perception for IoT Camera](exec-plans/active/phase9-bwe-perception.md) | 9 | **Active** — PR-1…PR-5 landed + post-review hardening + example-layer BWE coordinator extracted. Browser interop snapshot pending. | 1–2 sessions | TWCC parser + SDP/RTP wiring, loss-based BWE controller, runtime bounds/threshold API, send_fps + send_bitrate + fraction_lost stats, shared `examples/common/bwe_coordinator` glue, rk3588 `capture_set_bitrate()` consumer |
| [Phase 11: Send-side video RTP pacer](exec-plans/active/phase11-send-pacing.md) | 11 | **Active** — implemented; CI 42/42 + ASan + libdatachannel interop green; adversarial review done (1 fix, 1 → TD-023, now resolved in-branch via `nack_retx_buf`); pending browser smoke. | 1 session | BWE-driven leaky-bucket pacer (egress complement to #67 atomic admission), `NANORTC_PACING_*` tunables, `NANORTC_PACING_MAX_QUEUE_MS` latency cap, `test_video_pacing.c`, closes the Phase 9 BWE→egress loop |
| [Phase 12: Receive-path robustness](exec-plans/active/phase12-receive-robustness.md) | 12 | **Active** — PR-1 (auto-PLI) + PR-2 (opt-in reorder buffer) + PR-3 (opt-in receiver NACK) implemented; CI 44/44 + ASan + REORDER+NACK step + interop green + `fuzz_reorder`. Browser/real-link validation pending. | 2–3 sessions | Forward-gap → debounced auto-PLI + accurate `contiguous`; bounded reorder buffer (`nano_reorder.c`); receiver NACK → sender pkt_ring retransmit (full loss-recovery loop); surfaced & fixed TD-024/TD-025 |
| [Phase 13: FEC](exec-plans/active/phase13-fec.md) | 13 | **Active** — PR-1 codec + **PR-3/4 send+recv** + **NACK↔FEC coordination** (skip a NACK for an FEC-protected SN) + **adaptive group size K** (tracks TWCC loss: no FEC on clean links, more protection when lossy) implemented; loopback E2E (recovery / NACK-suppression / adaptive-K) + `fuzz_fec` + ASan-clean + CI `REORDER+NACK+FEC` 30/30 + libdatachannel interop; opt-in default-off. **PR-2 (SDP red/ulpfec + RED-wrap) + PR-5 (browser interop) pending** — shipped scheme is separate-SSRC ULPFEC (RFC 5109 §10.1 compliant, nanortc↔nanortc); RED-framing for Chrome = PR-2. | 3–4 sessions | Proactive zero-RTT loss recovery; ULPFEC level-0; adaptive overhead; ~24 KB opt-in; documented limits L1/L2/L3 (TD-026) |
| [Phase 14: Adaptive Media Controller](exec-plans/active/phase14-adaptive-media-controller.md) | 14 | **Active** — Phase A of the [adaptive-media roadmap](engineering/adaptive-media.md). **PR-1…PR-4 implemented + live-verified on H2O.** SDK pure-compute `nano_rate_control` + `nanortc_set_capability_ladder()` + `NANORTC_EV_SPEC_RECOMMENDATION`; example `capture_set_layout()` (FFmpeg/NVENC full 3-D reopen, GStreamer bitrate-only) + min-rung aggregation; host E2E + interop mid-stream-refresh tests + receiver contract. Adversarial review found 5 real issues in the unverifiable-on-mac reconfig code — all fixed (param-set UAF, etc.). Live: nvenc reopened 1080p→720p→360p without crash, browser decoded the switch, `tc` throttle drove BWE down + recovery. Up-step REMB-bounded (Phase B / #71). 13 host tests + 31/31 suite + ci-check --fast 17/17. opt-in, flag default-off. | 3–4 sessions | Resolution+fps+bitrate adaptation (not bitrate-only); pure-compute controller (belief #10); realtime/smoothness-first default policy; depends on Phase B/#71 for clean up-step |

**Total Phase 1-10:** ~25-32 agent sessions

## Completed plans

| Plan | Completed | Effort | Outcome |
|------|-----------|--------|---------|
| [0.2.0 convergence](engineering/nanortc-02.md) | 2026-10-13 | Implementation + validation | **Completed.** Explicit transmit results, authenticated DTLS, transactional negotiation, retained ICE restart, bounded feedback history and lifecycle/stats contracts. CI 54/54, ASan/UBSan 43/43, fresh fuzz, browser, ESP-IDF, resource and performance evidence; scoped runtime limits remain documented. |
| [DataChannel zero-retransmission integration](engineering/datachannel-zero-retransmit.md) | 2026-09-07 | Integration + validation | PR #79 adapted to #82 ownership; explicit zero-retry policy, protocol metadata, bounded gap-SACK abandonment; full CI, ASan/UBSan, million-run SCTP fuzz and four Chrome sessions passed. |
| [Design hardening and transport convergence](engineering/design-hardening.md) | 2026-09-07 | Implementation + review + validation | Bounded SCTP ownership/iterative reassembly, DCEP peek/commit, TURN shared scheduling and authenticated nonce transactions, instance logs, provider timers and length-delimited text; full CI, ASan/UBSan, SCTP/TURN million-run fuzz, 4 KiB interop, 4 Chrome sessions, 4 ESP-IDF examples and 7 P4 profiles passed. ESP32 hardware validation excluded by user. |
| [Phase 0: Skeleton](exec-plans/completed/phase0-skeleton.md) | 2026-03-26 | 1 session | 75 files, all 3 profiles build, 12 tests pass |
| [Phase 1: DataChannel E2E](exec-plans/completed/phase1-datachannel.md) | 2026-03-29 | 7 sessions | 5/5 interop tests, browser + ESP32-S3 DC verified, 140+ unit tests |
| [Phase 3: Video](exec-plans/completed/phase3-video.md) | 2026-04-05 | 2 sessions | H.264 FU-A + BWE + REMB, ESP32 camera (H.264 hw + Opus), Chrome verified |
| [Phase 4: Quality](exec-plans/completed/phase4-quality.md) | 2026-04-05 | 4 sessions | All 18 modules A grade, 7 fuzz harnesses (456M+ executions), Unity framework, 80%+ coverage, CI fuzz+coverage jobs |
| Phase 6: Resource Optimization | 2026-04-11 | 1 session | 34% RAM reduction (full-media 157→103 KB). Zero-copy CRC-32c, struct padding elimination, config default tuning, `NANORTC_FEATURE_TURN` flag, sizeof regression tests. SDP parser hardened (trailing whitespace trim). |
| [Phase 7: Stability & Performance Hardening](exec-plans/completed/phase7-stability-performance-hardening.md) | 2026-04-13 | 1 session | Fixed latent RTP receive scratch-buffer bug (C0) with compile-time regression guard. SRTP hot path: `inline srtp_compute_iv` + per-direction SSRC cache. SCTP padding byte-loops → `memset`. Overflow-safe subtraction guards in RTP/SRTP/H.264/DCEP parsers. Documented `NANORTC_MIN_POLL_INTERVAL_MS`. 768M fuzz execs clean, 4/4 libdatachannel interop pass. |
| [Phase 3.5: H.265/HEVC](exec-plans/completed/phase3-5-h265.md) | 2026-05-07 | 3 sessions | RFC 7798 Single NAL/AP/FU send+receive, `nanortc_video_set_h265_parameter_sets()` for sprop-vps/sps/pps. libdatachannel v0.22.5 interop (3 H.265 cases: Single NAL, sprop-* on answer, FU fragmentation). Fuzz `fuzz_h265`: 608M+ execs clean. 90% line coverage. Browser smoke tracked as follow-up. |
| [Phase 5: Network Traversal](exec-plans/completed/phase5-network-traversal.md) | 2026-05-17 | 4 sessions | Full TURN + ICE srflx + per-pair STUN transaction table. Phase 5.3 added the nanortc-as-TURN-client interop harness (5 strict cases in `test_interop_turn_relay_nanortc.c`) and fixed an RFC 6157 §4.2 perm-family-filter bug in `nano_rtc.c`. Strict assertions need a dual-host/external relay (single-host CI can't satisfy the three-distinct-endpoint requirement — loopback filtered by libjuice RFC 8838, same-host IP hits coturn hairpin); CI runs the test as compile + loopback-skip, strict path verified manually. Two-netns CI harness deferred. |
| [Phase 10: Design Convergence](exec-plans/completed/phase10-design-convergence.md) | 2026-05-08 | ~2 sessions | Docs convergence (design draft / ARCHITECTURE / QUALITY_SCORE / development-workflow), `nanortc_output_t` lifetime contract + `tests/test_output_lifetime.c` (default + `_min` ring variant) + ESP32-P4 nano hardware bench, `nanortc_next_timeout_ms()` deadline aggregator + `run_loop_{linux,esp}` integration, RTC orchestration split into `nano_rtc.c` (transport backbone) + `nano_rtc_media.c` + `nano_rtc_negotiate.c` sharing `src/nano_rtc_internal.h`. Sign-off CI 42/42 (7 combos × openssl/mbedtls + ASan + libdatachannel interop). |

## Technical Debt

Tracked in [tech-debt-tracker.md](exec-plans/tech-debt-tracker.md).

**Current status: 2 active items** — `TD-026` retains bounded FEC coverage
(shared groups, finite repair slots and one pending receive group); `TD-027` is
partially addressed by 0.2.0 but complete GCC probing and real-link/controller
validation remain open. See the tracker for the resolved historical defects
and current scope.

## Plan Lifecycle

1. **Draft** — Plan created, requirements being clarified
2. **Active** — Implementation in progress, decision log being maintained
3. **Completed** — All acceptance criteria met, moved to `completed/`
4. **Cancelled** — Superseded or no longer needed, reason documented
