# Technical Debt Tracker

Track known debt, prioritize by impact, pay down continuously.

## Active Debt

| ID | Category | Description | Impact | Priority | Plan to Resolve |
|----|----------|-------------|--------|----------|-----------------|
| TD-001 | Build | `-Wno-unused-parameter` suppresses useful warnings | Low | Phase 1 | Remove after stubs are replaced with real implementations |
| TD-002 | Test | No test framework — manual macros only | Low | Phase 4 | Evaluate Unity (embedded C test framework) if test count exceeds 50 |
| ~~TD-003~~ | ~~CI~~ | ~~No CI pipeline yet~~ | ~~Medium~~ | ~~Phase 1~~ | ~~Resolved~~ |
| TD-004 | Crypto | `nano_crypto_mbedtls.c` and `nano_crypto_openssl.c` are all stubs | High | Phase 1 Week 2 | Implement real crypto calls for DTLS + HMAC in both backends |
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
