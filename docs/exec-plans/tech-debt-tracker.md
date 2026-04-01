# Technical Debt Tracker

Track known debt, prioritize by impact, pay down continuously.

## Active Debt

| ID | Category | Description | Impact | Priority | Plan to Resolve |
|----|----------|-------------|--------|----------|-----------------|
| TD-001 | Build | `-Wno-unused-parameter` suppresses useful warnings | Low | Phase 3 | Remove per-file as stubs are replaced (all audio modules implemented, only BWE stub remains) |
| TD-002 | Test | No test framework — manual macros only (313 tests, exceeds 50 threshold) | Medium | Phase 3 | Evaluate Unity (embedded C test framework) — threshold reached |
| TD-008 | CI | `scripts/ci-check.sh` uses `declare -A` (bash 4+), fails on macOS default bash 3.2 | Low | Phase 3 | Replace associative array with positional args or install bash 4+ in CI |
| ~~TD-003~~ | ~~CI~~ | ~~No CI pipeline yet~~ | ~~Medium~~ | ~~Phase 1~~ | ~~Resolved~~ |
| ~~TD-004~~ | ~~Crypto~~ | ~~DTLS stubs remain in both backends~~ | ~~High~~ | ~~Phase 1 Step 2~~ | ~~Resolved~~ |
| ~~TD-005~~ | ~~Build~~ | ~~No `Kconfig` for ESP-IDF menuconfig~~ | ~~Low~~ | ~~Phase 1 Week 4~~ | ~~Resolved~~ |
| ~~TD-006~~ | ~~Examples~~ | ~~`run_loop_linux.c` and `signaling_stdin.c` untested with real network~~ | ~~Medium~~ | ~~Phase 1~~ | ~~Partially resolved~~ |
| ~~TD-007~~ | ~~Interop~~ | ~~SDP parser does not fully parse libdatachannel's SDP offer format~~ | ~~High~~ | ~~Phase 1~~ | ~~Resolved~~ |
| ~~TD-009~~ | ~~DataChannel~~ | ~~DCEP OPEN retransmit not deduplicated — each retransmit allocates new channel and re-emits open event~~ | ~~Medium~~ | ~~Phase 1~~ | ~~Resolved~~ |
| ~~TD-010~~ | ~~Crypto~~ | ~~mbedTLS 3.6 (ESP-IDF v5.x) build fails — `MBEDTLS_DEPRECATED_REMOVED` removes `pk_ec()`/`ecp_gen_key()`/`set_serial()`~~ | ~~High~~ | ~~Phase 1~~ | ~~Resolved~~ |
| ~~TD-011~~ | ~~Crypto~~ | ~~mbedTLS backend missing DTLS-SRTP `use_srtp` extension — Chrome silently drops all SRTP packets~~ | ~~High~~ | ~~Phase 2~~ | ~~Resolved~~ |
| ~~TD-012~~ | ~~SDP~~ | ~~`sdp_parse()` overwrites local `audio_pt` with remote offer's first PT — answerer sends wrong codec~~ | ~~High~~ | ~~Phase 2~~ | ~~Resolved~~ |
| ~~TD-013~~ | ~~ICE~~ | ~~`nanortc_add_remote_candidate()` returns `ERR_NOT_IMPLEMENTED` for IPv6 addresses — no IPv6 support~~ | ~~Medium~~ | ~~—~~ | ~~Resolved~~ |
| ~~TD-014~~ | ~~SDP~~ | ~~`rtc_apply_negotiated_media()` overwrites fmtp-selected H264 PT with m-line first PT (often VP8) — browser video decode fails~~ | ~~High~~ | ~~Phase 3~~ | ~~Resolved~~ |
| ~~TD-015~~ | ~~Config~~ | ~~`NANORTC_MEDIA_BUF_SIZE=1200` too small for RTP header (12) + max payload (1200) + SRTP tag (10) — video keyframe send fails~~ | ~~High~~ | ~~Phase 3~~ | ~~Resolved~~ |
| ~~TD-016~~ | ~~Examples~~ | ~~`browser_interop` and `linux_datachannel` pass event struct pointer to `nanortc_datachannel_send*()` instead of `nanortc_datachannel_t` handle~~ | ~~Medium~~ | ~~Phase 3~~ | ~~Resolved~~ |

## Resolved Debt

| ID | Resolved | Resolution |
|----|----------|-----------|
| TD-003 | 2026-03-26 | GitHub Actions CI + `scripts/ci-check.sh` — 3-profile × 2-crypto matrix, constraint checks, ASan, e2e tests |
| TD-004 | 2026-03-26 | DTLS fully implemented for both mbedtls 3.5 and OpenSSL 3.0 — ECDSA P-256 certs, BIO adapter, handshake, encrypt/decrypt, key export |
| TD-006 | 2026-03-27 | `run_loop_linux.c` validated via interop tests (nanortc peer wrapper uses it with real localhost UDP). `signaling_stdin.c` still untested — deferred to browser integration test. |
| TD-005 | 2026-03-29 | `Kconfig.projbuild` in `esp32_datachannel` example; `nanortc_config.h` maps `CONFIG_NANORTC_*` → `NANORTC_*` via Kconfig |
| TD-007 | 2026-03-27 | SDP parser fixed — ICE candidate parsing + `a=sendrecv` for libdatachannel compat (commit `4b5f7bb`). Audio m-line support added in Phase 2 Session 2. |
| TD-009 | 2026-03-30 | Idempotent DCEP OPEN handling — `dc_find_channel()` dedup before alloc, `last_was_open` flag gates event emission in `nano_rtc.c`. Re-ACKs retransmitted OPENs without duplicate channel or event. |
| TD-010 | 2026-03-30 | Added `NANORTC_MBEDTLS_3_6` version tier for mbedTLS 3.6+ (ESP-IDF v5.x). Uses PSA keygen (`psa_generate_key` + `pk_copy_from_psa`) and `set_serial_raw()` to avoid deprecated APIs removed by `MBEDTLS_DEPRECATED_REMOVED`. |
| TD-011 | 2026-03-30 | Added DTLS-SRTP `use_srtp` extension (`MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80`) to mbedTLS backend. OpenSSL backend already had it. Without this extension, Chrome/Firefox silently discard all SRTP packets. |
| TD-012 | 2026-03-30 | Refactored SDP codec negotiation: added `remote_audio_pt` field to separate parsed remote PT from local config PT. Removed `audio_sample_rate`/`audio_channels` parsing from rtpmap (local config is authoritative). |
| TD-013 | 2026-03-31 | Added `nano_addr.c/h` module with RFC 4291/5952 IPv6 parsing + formatting. Refactored `nanortc_add_remote_candidate()` to use `addr_parse_auto()` for dual-stack support. SDP `c=`/`o=` lines auto-select IP4/IP6. Guarded by `NANORTC_FEATURE_IPV6` (default ON). 48 address tests + IPv6 e2e + SDP tests. |
| TD-014 | 2026-03-31 | Refactored `rtc_apply_negotiated_media()`: video uses fmtp-selected PT (H264 with profile-level-id match), audio uses remote_pt. Added profile-level-id=42e01f preference in fmtp parsing. Debug logging at PT selection points. |
| TD-015 | 2026-03-31 | Changed `NANORTC_MEDIA_BUF_SIZE` from hardcoded 1200 to `(NANORTC_VIDEO_MTU + 80)`, derived from MTU constant. Provides headroom for RTP header (12) + SRTP tag (10) + alignment. |
| TD-016 | 2026-03-31 | Fixed `browser_interop` and `linux_datachannel` examples to use `nanortc_get_datachannel()` → `nanortc_datachannel_t` handle for send calls, matching the flat send API. Superseded 2026-04-01: removed handle type entirely, DataChannel API simplified to flat `nanortc_datachannel_send(rtc, id, ...)`. |

## Principles

1. **Pay continuously** — Address debt in small increments alongside feature work
2. **Track honestly** — Every known shortcut gets an entry here
3. **Prioritize by blast radius** — Debt that affects multiple modules or blocks progress gets fixed first
4. **Automate detection** — Where possible, add CI checks that catch new debt being introduced
