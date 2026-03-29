# Technical Debt Tracker

Track known debt, prioritize by impact, pay down continuously.

## Active Debt

| ID | Category | Description | Impact | Priority | Plan to Resolve |
|----|----------|-------------|--------|----------|-----------------|
| TD-001 | Build | `-Wno-unused-parameter` suppresses useful warnings | Low | Phase 3 | Remove per-file as stubs are replaced (all audio modules implemented, only BWE stub remains) |
| TD-002 | Test | No test framework — manual macros only (90+ tests, exceeds 50 threshold) | Medium | Phase 2 | Evaluate Unity (embedded C test framework) — threshold reached |
| ~~TD-003~~ | ~~CI~~ | ~~No CI pipeline yet~~ | ~~Medium~~ | ~~Phase 1~~ | ~~Resolved~~ |
| ~~TD-004~~ | ~~Crypto~~ | ~~DTLS stubs remain in both backends~~ | ~~High~~ | ~~Phase 1 Step 2~~ | ~~Resolved~~ |
| ~~TD-005~~ | ~~Build~~ | ~~No `Kconfig` for ESP-IDF menuconfig~~ | ~~Low~~ | ~~Phase 1 Week 4~~ | ~~Resolved~~ |
| ~~TD-006~~ | ~~Examples~~ | ~~`run_loop_linux.c` and `signaling_stdin.c` untested with real network~~ | ~~Medium~~ | ~~Phase 1~~ | ~~Partially resolved~~ |
| ~~TD-007~~ | ~~Interop~~ | ~~SDP parser does not fully parse libdatachannel's SDP offer format~~ | ~~High~~ | ~~Phase 1~~ | ~~Resolved~~ |

## Resolved Debt

| ID | Resolved | Resolution |
|----|----------|-----------|
| TD-003 | 2026-03-26 | GitHub Actions CI + `scripts/ci-check.sh` — 3-profile × 2-crypto matrix, constraint checks, ASan, e2e tests |
| TD-004 | 2026-03-26 | DTLS fully implemented for both mbedtls 3.5 and OpenSSL 3.0 — ECDSA P-256 certs, BIO adapter, handshake, encrypt/decrypt, key export |
| TD-006 | 2026-03-27 | `run_loop_linux.c` validated via interop tests (nanortc peer wrapper uses it with real localhost UDP). `signaling_stdin.c` still untested — deferred to browser integration test. |
| TD-005 | 2026-03-29 | `Kconfig.projbuild` in `esp32_datachannel` example; `nanortc_config.h` maps `CONFIG_NANORTC_*` → `NANORTC_*` via Kconfig |
| TD-007 | 2026-03-27 | SDP parser fixed — ICE candidate parsing + `a=sendrecv` for libdatachannel compat (commit `4b5f7bb`). Audio m-line support added in Phase 2 Session 2. |

## Principles

1. **Pay continuously** — Address debt in small increments alongside feature work
2. **Track honestly** — Every known shortcut gets an entry here
3. **Prioritize by blast radius** — Debt that affects multiple modules or blocks progress gets fixed first
4. **Automate detection** — Where possible, add CI checks that catch new debt being introduced
