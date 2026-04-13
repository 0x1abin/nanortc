# Execution Plans Index

Plans are first-class artifacts in NanoRTC. They are versioned, tracked, and co-located with the code.

## Effort Model

NanoRTC is built by AI coding agents. Estimates use **agent sessions** (one focused run, typically 2-6 hours of agent work) rather than human-weeks. The primary bottleneck is human review and browser-level verification, not coding speed.

## Active Plans

| Plan | Phase | Status | Effort | Target |
|------|-------|--------|--------|--------|
| [Phase 2: Audio](exec-plans/active/phase2-audio.md) | 2 | **Active** — Pending human verification of bidirectional audio + ESP32 intercom | 1 session | Bidirectional audio with browser |
| [Phase 5: Network Traversal](exec-plans/active/phase5-network-traversal.md) | 5 | **Active** — Sessions 1-4 complete. All acceptance criteria met. | 3 sessions | NAT traversal for production |
| [Phase 8: Continued Optimization & Stability](exec-plans/active/phase8-continued-optimization.md) | 8 | **Active** — Planning. 5 independent PRs (PR-1…PR-5) + P2 series on demand. | 4–6 sessions | IoT memory profile, stability propagation, video pkt_ring decoupling, H.264 zero-copy, ICE CONTROLLING per-pair transaction fix |

**Total Phase 1-8:** ~20-24 agent sessions

## Completed Plans

| Plan | Completed | Effort | Outcome |
|------|-----------|--------|---------|
| [Phase 0: Skeleton](exec-plans/completed/phase0-skeleton.md) | 2026-03-26 | 1 session | 75 files, all 3 profiles build, 12 tests pass |
| [Phase 1: DataChannel E2E](exec-plans/completed/phase1-datachannel.md) | 2026-03-29 | 7 sessions | 5/5 interop tests, browser + ESP32-S3 DC verified, 140+ unit tests |
| [Phase 3: Video](exec-plans/completed/phase3-video.md) | 2026-04-05 | 2 sessions | H.264 FU-A + BWE + REMB, ESP32 camera (H.264 hw + Opus), Chrome verified |
| [Phase 4: Quality](exec-plans/completed/phase4-quality.md) | 2026-04-05 | 4 sessions | All 18 modules A grade, 7 fuzz harnesses (456M+ executions), Unity framework, 80%+ coverage, CI fuzz+coverage jobs |
| Phase 6: Resource Optimization | 2026-04-11 | 1 session | 34% RAM reduction (full-media 157→103 KB). Zero-copy CRC-32c, struct padding elimination, config default tuning, `NANORTC_FEATURE_TURN` flag, sizeof regression tests. SDP parser hardened (trailing whitespace trim). |
| [Phase 7: Stability & Performance Hardening](exec-plans/completed/phase7-stability-performance-hardening.md) | 2026-04-13 | 1 session | Fixed latent RTP receive scratch-buffer bug (C0) with compile-time regression guard. SRTP hot path: `inline srtp_compute_iv` + per-direction SSRC cache. SCTP padding byte-loops → `memset`. Overflow-safe subtraction guards in RTP/SRTP/H.264/DCEP parsers. Documented `NANORTC_MIN_POLL_INTERVAL_MS`. 768M fuzz execs clean, 4/4 libdatachannel interop pass. |

## Technical Debt

Tracked in [tech-debt-tracker.md](exec-plans/tech-debt-tracker.md).

**Current status: 1 active item** — `TD-018` (ICE CONTROLLING per-pair transaction fix, Phase 8 / PR-5). 17 resolved.

## Plan Lifecycle

1. **Draft** — Plan created, requirements being clarified
2. **Active** — Implementation in progress, decision log being maintained
3. **Completed** — All acceptance criteria met, moved to `completed/`
4. **Cancelled** — Superseded or no longer needed, reason documented
