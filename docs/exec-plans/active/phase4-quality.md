# Phase 4: Quality & Robustness

**Status:** Active — Session 1 complete (SCTP gap tracking + DTLS close_notify)
**Estimated effort:** 3-4 agent sessions
**Goal:** All modules at A grade — fuzz-tested, browser-verified, interop-verified

## Acceptance Criteria

- [x] SCTP gap tracking (reorder buffer + gap ack blocks in SACK)
- [x] SCTP delivery queue for gap-fill batch delivery
- [x] DTLS close_notify via crypto provider (both backends)
- [ ] Fuzz testing for all parsers (STUN, SCTP, SDP, RTP, H.264)
- [ ] Code coverage measurement (gcov/lcov)
- [ ] nano_test.h enhancement (ASSERT_INT_EQ, SKIP, etc.)
- [ ] Core module line coverage >80%
- [ ] All modules at A grade in QUALITY_SCORE.md

## Session 1: SCTP Gap Tracking + DTLS close_notify — COMPLETE

| Task | File | Status |
|------|------|--------|
| Gap tracking reorder buffer | `nano_sctp.c/h` | Done — 7 new tests |
| Gap ack blocks in SACK encoding | `nano_sctp.c` | Done — `nsctp_encode_sack_with_gaps()` |
| Delivery queue for batch gap-fill | `nano_sctp.c/h`, `nano_rtc.c` | Done — `nsctp_poll_delivery()` |
| Config macros for gap buffer | `nanortc_config.h` | Done — `NANORTC_SCTP_MAX_RECV_GAP`, `NANORTC_SCTP_RECV_GAP_BUF_SIZE` |
| DTLS close_notify | `nano_dtls.c`, `nanortc_crypto.h` | Done — both OpenSSL + mbedTLS |
| close_notify test | `test_dtls.c` | Done — verifies Alert record (content type 21) |

**Gate:** 24/24 CI checks pass, 5/5 interop tests pass, all 6 feature combos build.

## Session 2: Fuzz Testing — PLANNED

| Task | File |
|------|------|
| libFuzzer harness — STUN | `tests/fuzz/fuzz_stun.c` |
| libFuzzer harness — SCTP | `tests/fuzz/fuzz_sctp.c` |
| libFuzzer harness — SDP | `tests/fuzz/fuzz_sdp.c` |
| libFuzzer harness — RTP/RTCP | `tests/fuzz/fuzz_rtp.c` |
| libFuzzer harness — H.264 | `tests/fuzz/fuzz_h264.c` |
| CMake `-DNANORTC_BUILD_FUZZ=ON` | `CMakeLists.txt` |
| Fuzz runner script | `scripts/run-fuzz.sh` |

## Session 3: Coverage + Test Enhancement — PLANNED

| Task | File |
|------|------|
| nano_test.h improvements | `tests/nano_test.h` |
| CMake `-DNANORTC_COVERAGE=ON` | `CMakeLists.txt` |
| Coverage report script | `scripts/coverage.sh` |
| Low-coverage path tests | `tests/test_*.c` |
