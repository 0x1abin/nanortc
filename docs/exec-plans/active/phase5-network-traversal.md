# Phase 5: Network Traversal

**Status:** Active — Sessions 1-4 complete. All acceptance criteria met.
**Estimated effort:** 4 agent sessions
**Goal:** Trickle ICE, ICE restart, TURN relay for production NAT traversal

## Acceptance Criteria

- [x] Trickle ICE: candidates can be added after SDP exchange
- [x] End-of-candidates signaling (RFC 8838)
- [x] ICE restart: new credentials, state reset (RFC 8445 §9)
- [x] Consent freshness (RFC 7675): periodic liveness checks
- [x] TURN Allocate + Refresh (RFC 5766)
- [x] TURN CreatePermission
- [x] TURN Send/Data indication relay
- [x] TURN ChannelBind + ChannelData framing (RFC 5766 §11)
- [x] Relay candidate in SDP (typ relay)
- [x] STUN srflx candidate discovery (RFC 8445 §5.1.1.1)
- [x] Srflx candidate in SDP (typ srflx)
- [x] Outgoing relay path wrapping (ChannelData/Send indication)
- [x] Incoming ChannelData demux (RFC 7983)
- [x] Permission + ChannelBind auto-refresh timers
- [x] Integration: TURN-relayed WebRTC session
- [x] Unit tests (24 TURN tests) + fuzz harness (fuzz_turn.c)
- [x] E2E interop test with coturn

## Session 1: Trickle ICE + ICE Restart

| Task | File | RFC |
|------|------|-----|
| Increase candidate limits (4→8, 8→12) | `nanortc_config.h` | — |
| Candidate type field (host/srflx/relay) | `nano_ice.h` | RFC 8445 |
| ICE restart API + state reset | `nano_ice.c` | RFC 8445 §9 |
| End-of-candidates handling | `nano_ice.c`, `nano_sdp.c` | RFC 8838 |
| Consent freshness timer | `nano_ice.c` | RFC 7675 |
| Trickle ICE event + public API | `nanortc.h` | RFC 8838 |
| FSM integration | `nano_rtc.c` | — |
| Tests | `test_trickle_ice.c` | — |

## Session 2: TURN Protocol Module

| Task | File | RFC |
|------|------|-----|
| TURN state machine | `nano_turn.h/c` (new) | RFC 5766 |
| STUN TURN attribute extensions | `nano_stun.h/c` | RFC 5766 §14 |
| Allocate/Refresh/CreatePermission | `nano_turn.c` | RFC 5766 §6-9 |
| Send/Data indication | `nano_turn.c` | RFC 5766 §10 |
| Fuzz harness | `fuzz_turn.c` | — |
| Unit tests | `test_turn.c` | — |

## Session 3: Integration + E2E

| Task | File |
|------|------|
| TURN in main FSM (timer, receive, send) | `nano_rtc.c` |
| Public API: nanortc_set_turn_server() | `nanortc.h` |
| Relay candidate SDP generation | `nano_sdp.c` |
| Interop test with coturn | `test_interop_turn.c` |
| Architecture docs update | `ARCHITECTURE.md` |

## Session 4: TURN E2E Interop Test with coturn

| Task | File |
|------|------|
| TURN interop test (3 cases: handshake, string, echo) | `test_interop_turn.c` |
| Unified nanortc peer start with optional ICE config | `interop_nanortc_peer.h/c` |
| TURN warmup phase (Allocate before signaling) | `interop_nanortc_peer.c` |
| Fix `turn_wrap_channel_data` symbol conflict with libjuice | `nano_turn.h/c`, `nano_rtc.c` |
| Environment variable fallback for TURN config | `browser_interop/main.c` |
| Register TURN test in CMake with `network` label | `tests/interop/CMakeLists.txt` |

## Phase 5.1: TURN compliance hardening + relay-only data-path verification

Discovered while auditing Phase 5: existing `test_interop_turn.c` connects libdc and nanortc via host candidates on localhost; the TURN allocation is a no-op background warmup, so `ChannelBind`, `ChannelData`, `Send Indication`, and `Data Indication` are not exercised by any e2e test. RFC 5766/8656 compliance was also not consolidated anywhere. See [docs/engineering/turn-rfc-compliance.md](../../engineering/turn-rfc-compliance.md) for the full review.

| Task | File |
|------|------|
| RFC 5766/8656/8489 compliance matrix + findings | `docs/engineering/turn-rfc-compliance.md` |
| F1: per-permission txid validation in `turn_handle_response()` | `src/nano_turn.{c,h}` |
| F2: tighten channel range to RFC 8656 0x4000–0x4FFE | `src/nano_turn.c` |
| F3: `turn_deallocate()` (Refresh with LIFETIME=0) | `src/nano_turn.{c,h}` |
| F5: parse `typ` attribute in `nanortc_add_remote_candidate()` (silently dropped before, masking RELAY/SRFLX everywhere) | `src/nano_rtc.c` |
| 8 new RFC-driven unit tests (channel range, ChannelData padding, Send-no-MI, CreatePerm/ChannelBind have MI, deallocate, txid spoofing, HMAC vector) | `tests/test_turn.c` |
| ICE config + relay-only injection for libdc peer (`iceTransportPolicy=RELAY`) | `tests/interop/interop_libdatachannel_peer.{c,h}` |
| 5 relay-only e2e tests (handshake, DC string, ChannelData burst, large payload, echo) | `tests/interop/test_interop_turn_relay.c` |
| Local coturn test infrastructure | `tests/interop/turn-server/{docker-compose.yml,turnserver.conf}`, `scripts/{start,stop}-test-turn.sh` |
| RFC 5766/8656/6156 added to RFC index | `docs/references/rfc-index.md` |

The relay-only e2e tests force `libdatachannel` into `RTC_TRANSPORT_POLICY_RELAY`, so the only candidate libdc advertises is its TURN-allocated relay. nanortc has no choice but to talk to libdc through the TURN server, exercising the full ChannelData / Send Indication / Data Indication data path on libdc's side and the relay-receive demux on nanortc's side. CTest label is `turn-relay`; tests skip cleanly if no TURN server is reachable.

## Decision Log

| # | Decision | Rationale |
|---|----------|-----------|
| 1 | No new feature flag for TURN | ~300B overhead acceptable; avoids build matrix expansion |
| 2 | Pre-computed TURN key (no MD5 in crypto interface) | Keeps crypto interface clean; user derives key externally |
| 3 | ~~Send/Data indication only~~ → ChannelBind fully implemented | Originally deferred; now complete with auto-refresh |
| 4 | SRFLX via lightweight state in nanortc_t (no separate module) | Only one request/response; ~100 lines, not worth a separate .c |
| 5 | Outgoing relay wrapping in `rtc_enqueue_transmit()` | Single-point abstraction for all outgoing paths; relay check is one conditional |
