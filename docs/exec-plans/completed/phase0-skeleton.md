# Phase 0: Project Skeleton

**Status:** Completed (2026-03-26)
**Duration:** 1 day

## Objective

Initialize the NanoRTC repository with a compilable skeleton, build system, test infrastructure, and documentation framework.

## Acceptance Criteria

- [x] All 13 source modules have stub implementations
- [x] CMakeLists.txt supports DATA/AUDIO/MEDIA profiles
- [x] Dual crypto backend: mbedtls (default) + OpenSSL (Linux host)
- [x] ESP-IDF component registration works
- [x] `test_main` passes for all 3 profiles × 2 crypto backends
- [x] `test_stun` stub passes
- [x] `test_e2e` end-to-end framework passes
- [x] CRC-32c implementation passes known test vector
- [x] AGENTS.md provides agent entry point (CLAUDE.md references it)
- [x] Architecture constraints documented and checked in CI
- [x] No forbidden includes in src/
- [x] No malloc in src/
- [x] GitHub Actions CI: 3 profiles × 2 crypto, constraints, ASan
- [x] Local CI: `scripts/ci-check.sh`
- [x] Linux examples: `linux_datachannel`, `linux_media_send`
- [x] Media sample data submodule (`examples/sample_data`)

## Decision Log

| Date | Decision | Rationale |
|------|----------|-----------|
| 2026-03-26 | Use `nano_rtc_t` as concrete struct (not opaque pointer) | Embedded targets need stack allocation; opaque pointers require heap |
| 2026-03-26 | CRC-32c uses lookup table, not bit-by-bit | ~30 lines, 1KB table, much faster on MCU |
| 2026-03-26 | No test framework dependency | Portability; simple macros suffice for embedded test runner |
| 2026-03-26 | `-Wno-unused-parameter` during stub phase | Stubs have intentionally unused params; will remove after Phase 1 |
| 2026-03-26 | Dual crypto backend (mbedtls + OpenSSL) | mbedtls for embedded, OpenSSL for Linux — both tested in CI |
| 2026-03-26 | ICE supports controlled + controlling roles | Device can be offerer or answerer |
| 2026-03-26 | AGENTS.md as primary agent file | Compatible with both Codex and Claude Code conventions |
| 2026-03-26 | Project name: NanoRTC in docs, nanortc in code | CamelCase for readability in prose, lowercase for C identifiers |

## Artifacts

- Repository skeleton: 75 files, 6006 lines
- Build system: CMake with 3-profile × 2-crypto support
- Test harness: custom macro-based (`nano_test.h`), 12 tests across 3 suites
- CI: GitHub Actions + local `scripts/ci-check.sh`
- Examples: Linux datachannel echo + media sender with sample data submodule
- Documentation: AGENTS.md, ARCHITECTURE.md, exec plans, quality scores, core beliefs, RFC index
