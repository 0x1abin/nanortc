# Execution Plans Index

Plans are first-class artifacts in NanoRTC. They are versioned, tracked, and co-located with the code.

## Effort Model

NanoRTC is built by AI coding agents. Estimates use **agent sessions** (one focused run, typically 2-6 hours of agent work) rather than human-weeks. The primary bottleneck is human review and browser-level verification, not coding speed.

## Active Plans

| Plan | Phase | Status | Effort | Target |
|------|-------|--------|--------|--------|
| [Phase 2: Audio](exec-plans/active/phase2-audio.md) | 2 | **Active** — Session 4 complete. Remaining: ESP32 audio intercom | 1 session | Bidirectional audio with browser |
| [Phase 3: Video](exec-plans/active/phase3-video.md) | 3 | **Active** — Session 2 complete. Remaining: ESP32 camera | 1 session | Camera streaming to browser |
| Phase 4: Quality | 4 | **Active** — Session 1 complete: SCTP gap tracking (7 tests), DTLS close_notify (1 test), `nano_rtc.c` delivery queue drain. Remaining: fuzz testing, coverage, test framework | 2-3 sessions | All modules A grade |

**Total Phase 1-4:** ~12-15 agent sessions

## Completed Plans

| Plan | Completed | Effort | Outcome |
|------|-----------|--------|---------|
| [Phase 0: Skeleton](exec-plans/completed/phase0-skeleton.md) | 2026-03-26 | 1 session | 75 files, all 3 profiles build, 12 tests pass |
| [Phase 1: DataChannel E2E](exec-plans/completed/phase1-datachannel.md) | 2026-03-29 | 7 sessions | 5/5 interop tests, browser + ESP32-S3 DC verified, 140+ unit tests |

## Technical Debt

Tracked in [tech-debt-tracker.md](exec-plans/tech-debt-tracker.md).

## Plan Lifecycle

1. **Draft** — Plan created, requirements being clarified
2. **Active** — Implementation in progress, decision log being maintained
3. **Completed** — All acceptance criteria met, moved to `completed/`
4. **Cancelled** — Superseded or no longer needed, reason documented
