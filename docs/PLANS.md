# Execution Plans Index

Plans are first-class artifacts in NanoRTC. They are versioned, tracked, and co-located with the code.

## Active Plans

| Plan | Phase | Status | Target |
|------|-------|--------|--------|
| [Phase 1: DataChannel E2E](exec-plans/active/phase1-datachannel.md) | 1 | **Active** | DataChannel working with browser |
| [Phase 2: Audio](exec-plans/active/phase2-audio.md) | 2 | Queued | Bidirectional audio with browser |
| [Phase 3: Video](exec-plans/active/phase3-video.md) | 3 | Queued | Camera streaming to browser |

## Completed Plans

| Plan | Completed | Outcome |
|------|-----------|---------|
| [Phase 0: Skeleton](exec-plans/completed/phase0-skeleton.md) | 2026-03-26 | All 3 profiles build, tests pass |

## Technical Debt

Tracked in [tech-debt-tracker.md](exec-plans/tech-debt-tracker.md).

## Plan Lifecycle

1. **Draft** — Plan created, requirements being clarified
2. **Active** — Implementation in progress, decision log being maintained
3. **Completed** — All acceptance criteria met, moved to `completed/`
4. **Cancelled** — Superseded or no longer needed, reason documented
