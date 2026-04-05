# Phase 5: Network Traversal

**Status:** Active — Session 1 in progress
**Estimated effort:** 3 agent sessions
**Goal:** Trickle ICE, ICE restart, TURN relay for production NAT traversal

## Acceptance Criteria

- [ ] Trickle ICE: candidates can be added after SDP exchange
- [ ] End-of-candidates signaling (RFC 8838)
- [ ] ICE restart: new credentials, state reset (RFC 8445 §9)
- [ ] Consent freshness (RFC 7675): periodic liveness checks
- [ ] TURN Allocate + Refresh (RFC 5766)
- [ ] TURN CreatePermission
- [ ] TURN Send/Data indication relay
- [ ] Relay candidate in SDP (typ relay)
- [ ] Integration: TURN-relayed WebRTC session
- [ ] Unit tests + fuzz harness for TURN

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

## Decision Log

| # | Decision | Rationale |
|---|----------|-----------|
| 1 | No new feature flag for TURN | ~300B overhead acceptable; avoids build matrix expansion |
| 2 | Pre-computed TURN key (no MD5 in crypto interface) | Keeps crypto interface clean; user derives key externally |
| 3 | Send/Data indication only (no ChannelBind) | Simpler MVP; ChannelBind deferred as optimization |
