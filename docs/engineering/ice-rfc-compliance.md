# ICE RFC Compliance Review

This document audits the NanoRTC ICE agent ([src/nano_ice.c](../../src/nano_ice.c), [src/nano_ice.h](../../src/nano_ice.h)) against:

- **RFC 8445** — Interactive Connectivity Establishment (ICE)
- **RFC 8489** — Session Traversal Utilities for NAT (STUN)
- **RFC 7675** — Session Traversal Utilities for NAT (STUN) Usage for Consent Freshness
- **RFC 8838** — Trickle ICE

Companion audits live in [turn-rfc-compliance.md](turn-rfc-compliance.md) (TURN) and the module tables in [../rfc-compliance-checklist.md](../rfc-compliance-checklist.md).

## Design Principles

NanoRTC's ICE implementation is **not** an exhaustive RFC 8445 reference build. It is tuned for three concrete targets — Chrome, Firefox, libdatachannel — on RTOS/embedded hardware (ESP32 class and up). The tables below distinguish implemented behavior from remaining limitations. Issue #81 exposed an incorrect controlled-role assumption; see [the implementation and validation record](issue-81-ice.md). These priorities do not make omitted RFC requirements compliant:

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
| 8445 §7.3.1.3 / §7.2.5.3.1 | Peer-reflexive candidates | Partial | Authenticated remote candidates use bounded existing slots. Additional local prflx mappings are deferred. |

### Priority & Pair Formation (RFC 8445 §5.1.2, §6.1.2)

| RFC §                 | Requirement                                                   | Status             | Implementation / Rationale |
|-----------------------|---------------------------------------------------------------|--------------------|----------------------------|
| 8445 §5.1.2.1         | `priority = (2^24)·type_pref + (2^8)·local_pref + (256 − component)` | OK          | [nano_ice.h:50-63](../../src/nano_ice.h) macros, type_pref host=126 / srflx=100 / relay=0 |
| 8445 §7.1.1 | STUN PRIORITY describes prospective prflx candidate | OK | `ICE_PRFLX_PRIORITY` uses type preference 110; SDP priorities describe the advertised candidate type. |
| 8445 §5.1.2.1         | Component ID distinct per RTP/RTCP                            | OK (pragmatic)     | Hardcoded to 1 (RTP). WebRTC mandates `a=rtcp-mux` so RTCP rides the same pair; multi-component ICE is unused. |
| 8445 §6.1.2.2 | Same-family pairs only | OK | The shared task reader filters families before selection. Incoming requests use the receiving socket or a same-family fallback; an absent family cannot be nominated. |
| 8445 §5.1.2           | tie-breaker is a cryptographically random 64-bit value        | OK                 | [nano_rtc.c:394+](../../src/nano_rtc.c) fills `ice.tie_breaker` via `cfg->crypto->random_bytes()` at init |
| 8445 §6.1.3           | Explicit per-pair state machine (Waiting / In-Progress / Frozen / Succeeded / Failed) | OK (pragmatic) | Agent-level 5 states + pending-transaction table instead of per-pair. Behavior is equivalent for ≤ 32 pairs (our `MAX_LOCAL × MAX_REMOTE` ceiling). |
| 8445 §6.1.1           | Explicit ordered check list                                   | OK (pragmatic)     | Round-robin via `current_local × current_remote` rotation. Same semantics for the small candidate counts we support. |

### Connectivity Checks (RFC 8445 §7)

| RFC §                 | Requirement                                                   | Status             | Implementation |
|-----------------------|---------------------------------------------------------------|--------------------|----------------|
| 8445 §6.1.1 / §7.2.4 | Both roles generate connectivity checks | OK | `ice_get_check` and `ice_send_check` share scheduling with RTC timeout queries. |
| 8445 §7.2.2           | USERNAME = `remote_ufrag:local_ufrag`                         | OK                 | [nano_ice.c:449-459](../../src/nano_ice.c) |
| 8445 §7.1.1, §8.1     | Binding Request carries FINGERPRINT                           | OK                 | Emitted by encoder, verified on incoming via mandatory-FP check |
| 8445 §7.2.2           | Connectivity check uses STUN short-term credential (MI)       | OK                 | Emitted by encoder; required on incoming via mandatory-MI check |
| 8445 §14              | Pacing interval Ta (default 50 ms)                            | OK                 | `NANORTC_ICE_CHECK_INTERVAL_MS` |
| 8445 §14 / 8489 §6.2.1 | Retransmit on timeout | OK within configured budget | Same transaction ID/content, exponential RTO, bounded expiry; active slots are never evicted to send a new check. |
| 8445 §7.2.5           | Binding Response: txid match                                  | OK                 | Per-pair pending table (TD-018); out-of-order responses tolerated |
| 8445 §7.2.5           | Binding Response MUST carry FINGERPRINT + MI                  | OK                 | [nano_ice.c](../../src/nano_ice.c) rejects responses missing either |
| 8445 §7.3             | Controlled: respond to Binding Request                        | OK                 | [nano_ice.c:177](../../src/nano_ice.c) |
| 8445 §7.3             | Verify USERNAME, FINGERPRINT, MI on incoming Request          | OK                 | All three enforced; `has_fingerprint` / `has_integrity` are hard fails |
| 8445 §7.3.1.5 | Controlled nomination requires successful check | OK | Incoming nomination is retained until the reverse check succeeds. DTLS starts afterward. |
| 8489 §9.1.5 | Binding Error Response authentication | OK | Match transaction and verify MI/FP before releasing it. Invalid UDP errors preserve pending state. |
| 8445 §7.3.1.1         | 487 Role Conflict → tie-breaker comparison → role swap        | Deferred           | Requires retransmit of all pending checks with flipped ICE-CONTROLLING/CONTROLLED. Browsers role-negotiate at offer/answer time, so 487 is never observed in WebRTC. |
| 8445 §7.3.1.4 | Triggered checks | Bounded | Pending slots hold queue flags; full-table admission is deferred without acknowledging an unretained nomination. |
| 8445 §8.1.1 | Regular nomination | OK | Controlling validates a pair, then sends USE-CANDIDATE in a new transaction. No aggressive-nomination toggle. |

### Consent Freshness (RFC 7675)

| RFC §                 | Requirement                                                   | Status             | Implementation |
|-----------------------|---------------------------------------------------------------|--------------------|----------------|
| 7675 §5.1             | Periodic STUN Binding Request on selected pair                | OK                 | [nano_ice.c:537](../../src/nano_ice.c); `NANORTC_ICE_CONSENT_INTERVAL_MS` = 15 s |
| 7675 §5.1             | Arm `consent_expiry_ms` on ICE connect                        | OK                 | [nano_rtc.c:1229](../../src/nano_rtc.c) at `state → CONNECTED` |
| 7675 §5.1 / 8445 §7.1.3 | Authenticate consent success before extending expiry        | OK                 | Transaction ID, FINGERPRINT, and MESSAGE-INTEGRITY are all verified; invalid responses leave `consent_pending` armed |
| 7675 §5.1             | Unarmed `consent_expiry_ms` surfaces as expired (not silently ignored) | OK        | `ice_consent_expired()` treats zero-while-CONNECTED as expired to prevent a forgotten-arm bug from disabling the liveness timeout |
| 7675 §5.2             | On consent expiry → transition to failure / DISCONNECTED       | OK                 | [nano_rtc.c](../../src/nano_rtc.c) emits `EV_DISCONNECTED` + state = CLOSED |
| 7675 §5.2             | No further checks generated after consent loss                | OK                 | `ice_generate_check` short-circuits on DISCONNECTED (and CONNECTED / FAILED) |

### Trickle ICE (RFC 8838) + Restart (RFC 8445 §9)

| RFC §                 | Requirement                                                   | Status             | Implementation |
|-----------------------|---------------------------------------------------------------|--------------------|----------------|
| 8838 §4               | Late remote candidates accepted via `nanortc_add_remote_candidate` | OK            | Array append up to `NANORTC_MAX_ICE_CANDIDATES` |
| 8838 §4               | `a=end-of-candidates` signal / API                            | OK                 | `ice.end_of_candidates` bit consumed in FAILED transition |
| 8445 §9               | `ice-restart` resets check state, preserves role + local candidates | OK           | [nano_ice.c:502](../../src/nano_ice.c); pending table zeroed; generation counter bumped |
| 8445 §9 / TURN peer state | Reuse live allocation but discard stale peer authorization | OK                 | `nanortc_ice_restart()` preserves the relay candidate/allocation and clears TURN permissions/channels for recreation from the new generation |

### Dual-Stack (RFC 8445 §6.1.2.2 + RFC 4291)

| RFC §                 | Requirement                                                   | Status             | Implementation |
|-----------------------|---------------------------------------------------------------|--------------------|----------------|
| 8445 §6.1.2.2 | Same-family pairs only | OK | The shared task reader filters families before selection. Incoming requests use the receiving socket or a same-family fallback; an absent family cannot be nominated. |
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

Unit tests ([tests/test_ice.c](../../tests/test_ice.c), [tests/test_trickle_ice.c](../../tests/test_trickle_ice.c)) cover the implemented subset. Deterministic NAT and current validation results are documented in [issue-81-ice.md](issue-81-ice.md). Highlights of the regression suite:

- `test_ice_request_without_fingerprint_rejected` / `test_ice_response_without_integrity_rejected` — mandatory MI + FP.
- `test_ice_unauthenticated_error_preserves_pending_slot` — invalid errors preserve live transactions.
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
| Additional local prflx mappings | §7.2.5.3.1 | Only already gathered mapped local candidates are reused; remote prflx admission is implemented. |
| 487 Role Conflict auto-swap | §7.3.1.1   | Browsers negotiate role at offer/answer; 487 has never been observed in interop testing.                    |
| Complete priority-sorted checklist and foundation unfreezing | §6.1.2 | A fixed active/valid table and round-robin ordinary checks bound memory; this is not a complete RFC checklist. |
| ICE TCP                     | §5.1.1.3   | Out of scope for UDP-only WebRTC subset.                                                                     |

These gaps limit compliance and interoperability; passing local and browser tests does not establish support for every NAT topology.
