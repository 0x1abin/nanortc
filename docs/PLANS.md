# Execution Plans Index

Plans are first-class artifacts in NanoRTC. They are versioned, tracked, and co-located with the code.

## Effort Model

NanoRTC is built by AI coding agents. Estimates use **agent sessions** (one focused run, typically 2-6 hours of agent work) rather than human-weeks. The primary bottleneck is human review and browser-level verification, not coding speed.

## Active Plans

| Plan | Phase | Status | Effort | Target |
|------|-------|--------|--------|--------|
| [Phase 1: DataChannel E2E](exec-plans/active/phase1-datachannel.md) | 1 | **Active** (Steps 1-2 done) | 2 of 4-6 sessions | DataChannel working with browser |
| [Phase 2: Audio](exec-plans/active/phase2-audio.md) | 2 | Queued | 2-3 sessions | Bidirectional audio with browser |
| [Phase 3: Video](exec-plans/active/phase3-video.md) | 3 | Queued | 2 sessions | Camera streaming to browser |

**Total Phase 1-3:** ~8-11 agent sessions (~1-2 weeks elapsed)

## Completed Plans

| Plan | Completed | Effort | Outcome |
|------|-----------|--------|---------|
| [Phase 0: Skeleton](exec-plans/completed/phase0-skeleton.md) | 2026-03-26 | 1 session | 75 files, all 3 profiles build, 12 tests pass |

## Technical Debt

Tracked in [tech-debt-tracker.md](exec-plans/tech-debt-tracker.md).

## Plan Lifecycle

1. **Draft** — Plan created, requirements being clarified
2. **Active** — Implementation in progress, decision log being maintained
3. **Completed** — All acceptance criteria met, moved to `completed/`
4. **Cancelled** — Superseded or no longer needed, reason documented
