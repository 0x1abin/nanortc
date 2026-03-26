# Technical Debt Tracker

Track known debt, prioritize by impact, pay down continuously.

## Active Debt

| ID | Category | Description | Impact | Priority | Plan to Resolve |
|----|----------|-------------|--------|----------|-----------------|
| TD-001 | Build | `-Wno-unused-parameter` suppresses useful warnings | Low | Phase 1 | Remove per-file as stubs are replaced (STUN/ICE done, DTLS/SCTP/DC remain) |
| TD-002 | Test | No test framework — manual macros only (69 tests, exceeds 50 threshold) | Medium | Phase 2 | Evaluate Unity (embedded C test framework) — threshold reached |
| ~~TD-003~~ | ~~CI~~ | ~~No CI pipeline yet~~ | ~~Medium~~ | ~~Phase 1~~ | ~~Resolved~~ |
| TD-004 | Crypto | DTLS stubs remain in both backends (hmac_sha1 + random_bytes done) | High | Phase 1 Step 2 | Implement dtls_init, dtls_handshake, dtls_encrypt/decrypt, certificate generation |
| TD-005 | Build | No `Kconfig` for ESP-IDF menuconfig | Low | Phase 1 Week 4 | Create Kconfig with profile + crypto selection |
| TD-006 | Examples | `run_loop_linux.c` and `signaling_stdin.c` untested with real network | Medium | Phase 1 Week 4 | Validate with browser integration test |

## Resolved Debt

| ID | Resolved | Resolution |
|----|----------|-----------|
| TD-003 | 2026-03-26 | GitHub Actions CI + `scripts/ci-check.sh` — 3-profile × 2-crypto matrix, constraint checks, ASan, e2e tests |

## Principles

1. **Pay continuously** — Address debt in small increments alongside feature work
2. **Track honestly** — Every known shortcut gets an entry here
3. **Prioritize by blast radius** — Debt that affects multiple modules or blocks progress gets fixed first
4. **Automate detection** — Where possible, add CI checks that catch new debt being introduced
