# Technical Debt Tracker

Track known debt, prioritize by impact, pay down continuously.

## Active Debt

| ID | Category | Description | Impact | Priority | Plan to Resolve |
|----|----------|-------------|--------|----------|-----------------|
| TD-001 | Build | `-Wno-unused-parameter` suppresses useful warnings | Low | Phase 1 | Remove after stubs are replaced with real implementations |
| TD-002 | Test | No test framework — manual macros only | Low | Phase 4 | Evaluate Unity (embedded C test framework) if test count exceeds 50 |
| ~~TD-003~~ | ~~CI~~ | ~~No CI pipeline yet~~ | ~~Medium~~ | ~~Phase 1~~ | ~~Resolved~~ |
| TD-004 | Crypto | `nano_crypto_mbedtls.c` is all stubs | High | Phase 1 Week 2 | Implement real mbedtls calls for DTLS + HMAC |
| TD-005 | Build | No `Kconfig` for ESP-IDF menuconfig | Low | Phase 1 Week 4 | Create Kconfig with profile selection |

## Resolved Debt

| ID | Resolved | Resolution |
|----|----------|-----------|
| TD-003 | 2026-03-26 | GitHub Actions CI + `scripts/ci-check.sh` — 3-profile build matrix, constraint checks, ASan, e2e tests |

## Principles

1. **Pay continuously** — Address debt in small increments alongside feature work
2. **Track honestly** — Every known shortcut gets an entry here
3. **Prioritize by blast radius** — Debt that affects multiple modules or blocks progress gets fixed first
4. **Automate detection** — Where possible, add CI checks that catch new debt being introduced
