# Phase 0: Project Skeleton

**Status:** Completed (2026-03-26)
**Duration:** 1 day

## Objective

Initialize the NanoRTC repository with a compilable skeleton, build system, test infrastructure, and documentation framework.

## Acceptance Criteria

- [x] All 13 source modules have stub implementations
- [x] CMakeLists.txt supports DATA/AUDIO/MEDIA profiles
- [x] ESP-IDF component registration works
- [x] `test_main` passes for all 3 profiles
- [x] `test_stun` stub passes
- [x] CRC-32c implementation passes known test vector
- [x] CLAUDE.md provides agent entry point
- [x] Architecture constraints are documented and checkable
- [x] No forbidden includes in src/
- [x] No malloc in src/

## Decision Log

| Date | Decision | Rationale |
|------|----------|-----------|
| 2026-03-26 | Use `nano_rtc_t` as concrete struct (not opaque pointer) | Embedded targets need stack allocation; opaque pointers require heap |
| 2026-03-26 | CRC-32c uses lookup table, not bit-by-bit | ~30 lines, 1KB table, much faster on MCU |
| 2026-03-26 | No test framework dependency | Portability; simple macros suffice for embedded test runner |
| 2026-03-26 | `-Wno-unused-parameter` during stub phase | Stubs have intentionally unused params; will remove after Phase 1 |

## Artifacts

- Repository skeleton: 35+ files
- Build system: CMake with 3-profile support
- Test harness: custom macro-based (no external deps)
- Documentation: CLAUDE.md, ARCHITECTURE.md, workflow docs, core beliefs
