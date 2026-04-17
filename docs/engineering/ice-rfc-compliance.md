# ICE RFC Compliance Review

This document audits the NanoRTC ICE agent ([src/nano_ice.c](../../src/nano_ice.c), [src/nano_ice.h](../../src/nano_ice.h)) against:

- **RFC 8445** — Interactive Connectivity Establishment (ICE)
- **RFC 8489** — Session Traversal Utilities for NAT (STUN)
- **RFC 7675** — Session Traversal Utilities for NAT (STUN) Usage for Consent Freshness
- **RFC 8838** — Trickle ICE

Companion audits live in [turn-rfc-compliance.md](turn-rfc-compliance.md) (TURN) and the module tables in [../rfc-compliance-checklist.md](../rfc-compliance-checklist.md).

## Design Principles

NanoRTC's ICE implementation is **not** an exhaustive RFC 8445 reference build. It is tuned for three concrete targets — Chrome, Firefox, libdatachannel — on RTOS/embedded hardware (ESP32 class and up). Every deviation from RFC 8445 in the sections below is a deliberate trade-off that falls out of these priorities:

1. **Interop with mainstream WebRTC peers first.** If a requirement is something browsers and libdatachannel actually exercise on real connections, we implement it. If it is an optional RFC feature that no shipping WebRTC stack enforces, we may defer it.
2. **Embedded resource ceilings are hard.** `sizeof(nano_ice_t)` stays under the 600 B ceiling (see [memory-profiles.md](memory-profiles.md)), `NANORTC_MAX_LOCAL_CANDIDATES` is 4, `NANORTC_MAX_ICE_CANDIDATES` is 8, `NANORTC_ICE_MAX_PENDING_CHECKS` is 4. Adding RFC features that require unbounded per-pair state or a full peer-reflexive candidate cache is weighed against these ceilings.
3. **Sans-I/O + no dynamic allocation.** Anything that needs `malloc` or a platform socket is by construction unacceptable (`AGENTS.md` mandatory rule). This drops the door on a handful of RFC-optional features (e.g. ICE TCP, IPv4/IPv6 dual allocation over a single TURN server).
4. **Correctness of what we do implement beats breadth.** All the attributes / message types / state transitions we emit are on-spec and verified against browsers; the boundary is not "partially broken features" but "feature is either fully present or explicitly absent".

What this means for readers of this table: `OK` means on-spec and interop-verified. `OK (pragmatic)` means we implement the intent but under a simplifying assumption valid for the WebRTC subset. `Deferred` means RFC-conformant but unimplemented by choice, with the rationale stated.

## Compliance Matrix

### Candidate Types (RFC 8445 §5.1.1)

| RFC §                 | Requirement                                                   | Status             | Implementation / Rationale |
|-----------------------|---------------------------------------------------------------|--------------------|----------------------------|
| 8445 §5.1.1           | Host candidates                                               | OK                 | [nano_ice.h:44](../../src/nano_ice.h), registered via `nanortc_add_local_candidate` |
| 8445 §5.1.1           | Server-reflexive (srflx) via STUN                             | OK                 | Gated by `NANORTC_FEATURE_ICE_SRFLX`; registered after STUN binding reply |
| 8445 §5.1.1           | Relayed via TURN                                              | OK                 | Gated by `NANORTC_FEATURE_TURN`; see [turn-rfc-compliance.md](turn-rfc-compliance.md) |
| 8445 §5.1.1.2         | Peer-reflexive (prflx) candidate registration                 | Deferred           | XOR-MAPPED-ADDRESS is parsed but not materialised into a new local/remote candidate. Mainstream peers converge without prflx on home/office NATs; adding it costs a dynamic candidate slot and a triggered-check queue. |

### Priority & Pair Formation (RFC 8445 §5.1.2, §6.1.2)

| RFC §                 | Requirement                                                   | Status             | Implementation / Rationale |
|-----------------------|---------------------------------------------------------------|--------------------|----------------------------|
| 8445 §5.1.2.1         | `priority = (2^24)·type_pref + (2^8)·local_pref + (256 − component)` | OK          | [nano_ice.h:50-63](../../src/nano_ice.h) macros, type_pref host=126 / srflx=100 / relay=0 |
| 8445 §5.1.2.1         | SDP `a=candidate` priority matches STUN PRIORITY attribute    | OK                 | [nano_sdp.c](../../src/nano_sdp.c) emits srflx/relay priorities via `ICE_SRFLX_PRIORITY` / `ICE_RELAY_PRIORITY` with the same idx the runtime will use |
| 8445 §5.1.2.1         | Component ID distinct per RTP/RTCP                            | OK (pragmatic)     | Hardcoded to 1 (RTP). WebRTC mandates `a=rtcp-mux` so RTCP rides the same pair; multi-component ICE is unused. |
| 8445 §6.1.2.2         | Pairs MUST be same-address-family                             | OK                 | `ice_advance_to_same_family_pair` in [nano_ice.c:132](../../src/nano_ice.c) skips cross-family slots before burning a check budget |
| 8445 §5.1.2           | tie-breaker is a cryptographically random 64-bit value        | OK                 | [nano_rtc.c:394+](../../src/nano_rtc.c) fills `ice.tie_breaker` via `cfg->crypto->random_bytes()` at init |
| 8445 §6.1.3           | Explicit per-pair state machine (Waiting / In-Progress / Frozen / Succeeded / Failed) | OK (pragmatic) | Agent-level 5 states + pending-transaction table instead of per-pair. Behavior is equivalent for ≤ 32 pairs (our `MAX_LOCAL × MAX_REMOTE` ceiling). |
| 8445 §6.1.1           | Explicit ordered check list                                   | OK (pragmatic)     | Round-robin via `current_local × current_remote` rotation. Same semantics for the small candidate counts we support. |

### Connectivity Checks (RFC 8445 §7)

| RFC §                 | Requirement                                                   | Status             | Implementation |
|-----------------------|---------------------------------------------------------------|--------------------|----------------|
| 8445 §7.1.1           | Controlling: generate STUN Binding Request                    | OK                 | [nano_ice.c:372](../../src/nano_ice.c) |
| 8445 §7.1.1           | USERNAME = `remote_ufrag:local_ufrag`                         | OK                 | [nano_ice.c:449-459](../../src/nano_ice.c) |
| 8445 §7.1.1, §8.1     | Binding Request carries FINGERPRINT                           | OK                 | Emitted by encoder, verified on incoming via mandatory-FP check |
| 8445 §7.1.2.1         | Connectivity check uses STUN short-term credential (MI)       | OK                 | Emitted by encoder; required on incoming via mandatory-MI check |
| 8445 §7.1.2           | Pacing interval Ta (default 50 ms)                            | OK                 | `NANORTC_ICE_CHECK_INTERVAL_MS` |
| 8445 §6.1.4.2         | Retransmit on timeout                                         | OK                 | Stale pending slot reaped after `NANORTC_ICE_CHECK_TIMEOUT_MS` |
| 8445 §7.1.3           | Binding Response: txid match                                  | OK                 | Per-pair pending table (TD-018); out-of-order responses tolerated |
| 8445 §7.1.3           | Binding Response MUST carry FINGERPRINT + MI                  | OK                 | [nano_ice.c](../../src/nano_ice.c) rejects responses missing either |
| 8445 §7.2.1           | Controlled: respond to Binding Request                        | OK                 | [nano_ice.c:177](../../src/nano_ice.c) |
| 8445 §7.2.1.1         | Verify USERNAME, FINGERPRINT, MI on incoming Request          | OK                 | All three enforced; `has_fingerprint` / `has_integrity` are hard fails |
| 8445 §7.2.1.4         | USE-CANDIDATE nominates and transitions to CONNECTED          | OK                 | [nano_ice.c:220-264](../../src/nano_ice.c) |
| 8489 §6.3.4           | Binding Error Response (0x0111) handling                      | OK (pragmatic)     | Matches txid, frees pending slot, returns ERR_PROTOCOL. Full 487 Role Conflict auto-swap (§7.3.1.1) is deferred — both peers as controlling is a configuration error browsers never make. |
| 8445 §7.3.1.1         | 487 Role Conflict → tie-breaker comparison → role swap        | Deferred           | Requires retransmit of all pending checks with flipped ICE-CONTROLLING/CONTROLLED. Browsers role-negotiate at offer/answer time, so 487 is never observed in WebRTC. |
| 8445 §7.2.1.3         | Triggered check queue                                         | Deferred           | Incoming Binding Request gets an immediate response; no priority-elevation queue. Convergence stays within `NANORTC_ICE_MAX_CHECKS` for realistic pair counts. |
| 8445 §8               | Regular vs aggressive nomination toggle                       | OK (pragmatic)     | Aggressive hardcoded (USE-CANDIDATE on every check). RFC permits; converges faster, costs one extra attribute per request. |

### Consent Freshness (RFC 7675)

| RFC §                 | Requirement                                                   | Status             | Implementation |
|-----------------------|---------------------------------------------------------------|--------------------|----------------|
| 7675 §5.1             | Periodic STUN Binding Request on selected pair                | OK                 | [nano_ice.c:537](../../src/nano_ice.c); `NANORTC_ICE_CONSENT_INTERVAL_MS` = 15 s |
| 7675 §5.1             | Arm `consent_expiry_ms` on ICE connect                        | OK                 | [nano_rtc.c:1229](../../src/nano_rtc.c) at `state → CONNECTED` |
| 7675 §5.1             | Unarmed `consent_expiry_ms` surfaces as expired (not silently ignored) | OK        | `ice_consent_expired()` treats zero-while-CONNECTED as expired to prevent a forgotten-arm bug from disabling the liveness timeout |
| 7675 §5.2             | On consent expiry → transition to failure / DISCONNECTED       | OK                 | [nano_rtc.c](../../src/nano_rtc.c) emits `EV_DISCONNECTED` + state = CLOSED |
| 7675 §5.2             | No further checks generated after consent loss                | OK                 | `ice_generate_check` short-circuits on DISCONNECTED (and CONNECTED / FAILED) |

### Trickle ICE (RFC 8838) + Restart (RFC 8445 §9)

| RFC §                 | Requirement                                                   | Status             | Implementation |
|-----------------------|---------------------------------------------------------------|--------------------|----------------|
| 8838 §4               | Late remote candidates accepted via `nanortc_add_remote_candidate` | OK            | Array append up to `NANORTC_MAX_ICE_CANDIDATES` |
| 8838 §4               | `a=end-of-candidates` signal / API                            | OK                 | `ice.end_of_candidates` bit consumed in FAILED transition |
| 8445 §9               | `ice-restart` resets check state, preserves role + local candidates | OK           | [nano_ice.c:502](../../src/nano_ice.c); pending table zeroed; generation counter bumped |

### Dual-Stack (RFC 8445 §6.1.2.2 + RFC 4291)

| RFC §                 | Requirement                                                   | Status             | Implementation |
|-----------------------|---------------------------------------------------------------|--------------------|----------------|
| 8445 §6.1.2.2         | Same-family pairs only                                        | OK                 | `ice_advance_to_same_family_pair` for outbound check pairing; `ice_find_local_idx_by_family` resolves the same-family local candidate when controlled-side USE-CANDIDATE arrives without an interface hint (`dst.family==0` from a wildcard-socket caller), preventing dual-stack hosts from latching the wrong-family idx 0. |
| 8489 §14.2            | STUN XOR-MAPPED-ADDRESS for IPv4 and IPv6                     | OK                 | `stun_decode_xor_addr` handles both families |
| 4291 / 5952           | IPv6 address parse + format                                   | OK                 | `src/nano_addr.c` RFC 5952 canonical form, 70M+ fuzz execs clean |
| 6156 §4               | TURN REQUESTED-ADDRESS-FAMILY                                  | Out of scope       | Explicit per [rfc-index.md](../references/rfc-index.md). TURN server family is taken as-is. |

### Address Family — Examples / Host Integration

| Capability                                    | Status | Location |
|-----------------------------------------------|--------|----------|
| Linux run-loop enumerates AF_INET + AF_INET6 | OK | [examples/common/run_loop_linux.c](../../examples/common/run_loop_linux.c) |
| `browser_interop` registers global IPv6 host addresses | OK | [examples/browser_interop/main.c](../../examples/browser_interop/main.c) |
| ESP-IDF run-loop IPv6 enumeration | Deferred | lwIP configuration-specific; planned as follow-up |
| Link-local (`fe80::/10`) ICE candidates | Out of scope | Requires `scope_id` propagation through the ICE wire format; browsers use mDNS (`.local.`) instead. |

## Test Coverage

Unit tests ([tests/test_ice.c](../../tests/test_ice.c), [tests/test_trickle_ice.c](../../tests/test_trickle_ice.c)) cover every `OK` row. Highlights of the regression suite:

- `test_ice_request_without_fingerprint_rejected` / `test_ice_response_without_integrity_rejected` — mandatory MI + FP.
- `test_ice_binding_error_frees_pending_slot` — 0x0111 slot accounting.
- `test_ice_controlling_multi_pair_response_out_of_order` / `test_ice_controlling_pending_table_full` — pending transaction table (TD-018).
- `test_ice_pair_family_filter_*` — RFC 8445 §6.1.2.2 outbound pair selection.
- `test_ice_controlled_dual_stack_local_fallback` / `test_ice_controlled_single_v4_local_fallback_keeps_idx_0` — RFC 8445 §6.1.2.2 same-family fallback on the controlled-side nomination path when `dst.family==0`.
- `test_e2e_tie_breaker_is_randomised` — §5.2 random tie-breaker.
- `test_e2e_ipv6_loopback_connects` — end-to-end ICE + DTLS over `[::1]`.
- `test_consent_expired_when_unarmed` — unarmed timer surfaces as expired.
- `test_ice_generate_check_noop_in_disconnected` — no checks after consent loss.

End-to-end verification:
- libdatachannel interop suite (5 tests, coturn + host relay).
- Chrome / Firefox `browser_interop` example, manually verified.
- Fuzz: `fuzz_stun` 76M+ executions clean.

## Known Gaps (Deferred, with Rationale)

| Gap                         | Category   | Rationale                                                                                                   |
|-----------------------------|------------|-------------------------------------------------------------------------------------------------------------|
| Peer-reflexive candidate    | §5.1.1.2   | Needs a dynamic candidate slot + triggered-check queue. WebRTC peers converge on common NATs without it.    |
| 487 Role Conflict auto-swap | §7.3.1.1   | Browsers negotiate role at offer/answer; 487 has never been observed in interop testing.                    |
| Explicit per-pair states    | §6.1.3     | Agent-level 5 states cover every transition we need for ≤ 32 pairs. Adding per-pair state grows `nano_ice_t`. |
| Triggered check queue       | §7.2.1.3   | Immediate response is always sent; convergence stays within budget for realistic candidate counts.           |
| Regular nomination toggle   | §8         | Aggressive works, converges faster, no configuration surface required.                                       |
| ICE TCP                     | §5.1.1.3   | Out of scope for UDP-only WebRTC subset.                                                                     |

Each of these is revisitable if real-world interop data shows a peer failing to connect; the design principle is "fix what breaks, not what an exhaustive RFC reading says we might fix".
