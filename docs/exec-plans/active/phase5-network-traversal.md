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

## Phase 5.2: Client-side relay data path (nanortc-as-TURN-client) — Apr 2026

Phase 5.1 covered nanortc's *server-side* TURN code (libdc relays, nanortc receives via Data Indication / ChannelData) but never drove nanortc's own outbound media wrap over a real cellular path. Discovered while bringing up the macOS uipcat-camera SDK example: `viewer connected` fired on cellular, but no media flowed. F6 was the hidden showstopper — `selected_type` never became `RELAY` because the controlled side could not tell a TURN-tunneled USE-CANDIDATE check from a direct one. F7/F8/F9 piled up as soon as F6 was unblocked. See [docs/engineering/turn-rfc-compliance.md](../../engineering/turn-rfc-compliance.md) Phase 5.2 section for the full finding table.

| Task | File |
|------|------|
| F6: plumb `bool via_turn` through `rtc_process_receive` and `ice_handle_stun`; set `selected_type=RELAY` on USE-CANDIDATE when via_turn | `src/nano_ice.{c,h}`, `src/nano_rtc.c`, `tests/test_ice.c`, `tests/test_trickle_ice.c` |
| F7: defer TURN wrap to `nanortc_poll_output()` via `out_wrap_meta[]` side table; dedicated `turn_buf` separate from `stun_buf` | `src/nano_rtc.c`, `include/nanortc.h`, `include/nanortc_config.h` (`NANORTC_TURN_BUF_SIZE`) |
| F8: route ICE consent freshness checks (`ice_generate_consent`) through `rtc_enqueue_transmit()` so they get wrapped on RELAY pairs | `src/nano_rtc.c` |
| F9: per-tick `CreatePermission` fan-out from `rtc_process_timers` covering all `remote_candidates[]` (not just `[0]`); picks up trickle additions next tick | `src/nano_rtc.c` |
| F10: `stats_enqueue_via_turn` / `_direct` / `wrap_dropped` / `tx_queue_full` counters on `nanortc_t` for runtime observability of the wrap path | `include/nanortc.h`, `src/nano_rtc.c` |
| Refactor `rtc_enqueue_transmit` to delegate to `rtc_enqueue_output` (no duplicated queue-management code) | `src/nano_rtc.c` |
| `test_sizeof.c` bound relaxed for non-default `NANORTC_MAX_ICE_CANDIDATES` overrides | `tests/test_sizeof.c` |
| New `T19a` test: `nanortc_add_remote_candidate()` parses `typ` host/srflx/relay (regression for F5) | `tests/test_trickle_ice.c` |

Manual verification path (no automated test exists yet — see Coverage gap below): macOS uipcat-camera SDK example, cellular viewer, observed at `EV_CONNECTED`:

```
ICE selected pair type=relay  remote=211.90.236.246:60395
turn_st=ALLOCATED perm=3 chan=0
tx_via_turn=8  tx_direct=0  wrap_drop=0  q_full=0
```

**Coverage gap (still open):** `tests/interop/test_interop_turn_relay.c` only validates the server-side direction. A new harness is needed where nanortc is forced into relay-only mode while libdc has only host candidates, asserting `ice.selected_type == NANORTC_ICE_CAND_RELAY` and `stats_enqueue_via_turn > 0`. Tracked as Phase 5.3 / `test_interop_turn_relay_nanortc.c`.

## Decision Log

| # | Decision | Rationale |
|---|----------|-----------|
| 1 | No new feature flag for TURN | ~300B overhead acceptable; avoids build matrix expansion |
| 2 | Pre-computed TURN key (no MD5 in crypto interface) | Keeps crypto interface clean; user derives key externally |
| 3 | ~~Send/Data indication only~~ → ChannelBind fully implemented | Originally deferred; now complete with auto-refresh |
| 4 | SRFLX via lightweight state in nanortc_t (no separate module) | Only one request/response; ~100 lines, not worth a separate .c |
| 5 | Outgoing relay wrapping in `rtc_enqueue_transmit()` | Single-point abstraction for all outgoing paths; relay check is one conditional |
| 6 | Lazy TURN wrap (defer to `nanortc_poll_output()`) instead of per-slot wrap buffers | Per-slot scratch needs ~384 KB for `NANORTC_OUT_QUEUE_SIZE=256 × ~1500 B`; unacceptable for embedded. Lazy wrap serialises ChannelData / Send-indication encoding through a single `turn_buf` at dispatch time, costing only the side-table (`out_wrap_meta[]` ≈ 24 B/slot). |
| 7 | `via_turn` plumbed through `rtc_process_receive` → `ice_handle_stun` instead of a permission-existence heuristic | Routing every peer with a permission through TURN broke same-LAN responses (TURN server's source IP doesn't match the local pair the browser sent the check on, so the response is dropped). The signal must be "did this packet just come out of a TURN unwrap" — only `rtc_process_receive` knows that, so it's plumbed explicitly rather than guessed downstream. |
