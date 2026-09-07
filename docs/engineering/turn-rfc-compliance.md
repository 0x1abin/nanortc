# TURN RFC Compliance Review

This document audits the NanoRTC TURN client implementation ([src/nano_turn.c](../../src/nano_turn.c), [src/nano_turn.h](../../src/nano_turn.h)) against:

- **RFC 5766** — Traversal Using Relays around NAT (TURN)
- **RFC 8656** — Traversal Using Relays around NAT (TURN-bis), obsoletes 5766 and 6156
- **RFC 8489** — Session Traversal Utilities for NAT (STUN), the TURN authentication base
- **RFC 7983** — Multiplexing Scheme Updates for DTLS-SRTP (ChannelData demux)

NanoRTC targets the WebRTC-relevant TURN subset: UDP transport, long-term credential authentication, ChannelBind for low-overhead relaying. Optional TURN features (TCP/TLS transport, EVEN-PORT, address-family negotiation) are out of scope.

## Current implementation and limits

The matrix covers the implemented UDP subset, not full RFC 8656 compliance.
Credentials use legacy MD5 key derivation and HMAC-SHA1 MESSAGE-INTEGRITY.
SHA-256, PASSWORD-ALGORITHMS negotiation and USERHASH are not implemented.

| RFC sections | Implemented behavior | Owning functions |
|---|---|---|
| 5766 §6 / 8656 §7 | Allocate, UDP REQUESTED-TRANSPORT, LIFETIME, 401 REALM/NONCE challenge, relay address extraction | `turn_start_allocate`, `turn_handle_response` |
| 5766 §7 / 8656 §8 | Authenticated Refresh before expiry; explicit LIFETIME=0 deallocation | `turn_generate_refresh`, `turn_deallocate` |
| 5766 §§8–9 / 8656 §§9–10 | IP-scoped permissions; five-minute lifetime, four-minute refresh; independent pending requests and peer-scoped rejection/backoff | `turn_create_permission`, `turn_can_create_permission`, `turn_peer_is_ready` |
| 5766 §10 / 8656 §11 | Send Indication without MI; Data Indication peer/payload extraction | `turn_wrap_send`, `turn_unwrap_data` |
| 5766 §11 / 8656 §12 | ChannelBind keyed by peer endpoint, channel range 0x4000–0x4FFE, ten-minute binding refreshed at nine minutes; ChannelData encode/decode | `turn_channel_bind`, `turn_can_bind_channel`, `turn_wrap_channel_data`, `turn_unwrap_channel_data` |
| 8489 §§5, 6.2.1 | Same bytes/ID for retransmission, bounded exponential backoff; changed nonce retires IDs before rebuilding requests | `turn_store_nonce`, `turn_generate_retransmit` |
| 8489 §§9.2.2, 9.2.5, 14.5, 14.7 | Legacy key derivation; validate response MI before error handling, discard invalid UDP responses; verify FINGERPRINT when present | `turn_derive_key`, `turn_handle_response` |
| 8656 §§8–12 | Shared task/deadline selection; outstanding requests wait for RTO instead of expired refresh deadlines | `turn_next_work`, `turn_poll_output`, `turn_next_timeout_ms` |
| 7983 §3 | STUN/ChannelData demultiplexing | RTC receive path and `turn_is_channel_data` |

Implementation: [nano_turn.c](../../src/nano_turn.c),
[nano_turn.h](../../src/nano_turn.h), [nano_rtc.c](../../src/nano_rtc.c).
Authentication rules are checked against
[RFC 8489 §9.2.5](https://www.rfc-editor.org/rfc/rfc8489.html#section-9.2.5);
TURN operation/attribute references use the
[RFC 8656 section numbering](https://www.rfc-editor.org/rfc/rfc8656.html#section-7).

### Deliberate omissions

| RFC sections | Feature | Current boundary |
|---|---|---|
| 8656 §§18.7, 18.10 | EVEN-PORT / RESERVATION-TOKEN | No paired relay-port reservation |
| 8656 §§18.6, 18.11 | REQUESTED-ADDRESS-FAMILY / ADDITIONAL-ADDRESS-FAMILY | No relay address-family negotiation; IPv4 allocation |
| 8656 §18.9 | DONT-FRAGMENT | No requested DF control |
| RFC 6062 | TCP allocations | Peer leg is UDP |
| 8489 §6.2 | TCP/TLS/DTLS client-server transport | Client-server TURN transport is UDP |
| RFC 7635 | OAuth third-party authorization | Long-term credentials only |

## Transport convergence (Sep 2026)

Reviewing PRs #72, #73, #75, #76, #78 and the uncommitted hardening work
reproduced three failures: a Refresh waiting another 400 ms returned timeout 0;
a concurrent request reused its ID after a shared nonce change altered its
bytes; and an unsigned 437 matching an authenticated transaction failed the
allocation. The first caused event-loop spinning, the second violated transaction
identity, and the third dispatched an unauthenticated error.

TURN now owns the task selector used by both output and timeout queries. RTC
retains candidate order and output-slot admission, and asks TURN about peer
readiness instead of inspecting permission/channel internals. Existing bounded
fan-out, peer-specific failures and retry backoff remain in place.

Authentication precedes error dispatch. For authenticated transactions only
401/438 challenges may omit MI; if MI is supplied it must validate. Other UDP
responses with missing/invalid MI are discarded without changing live state.
A nonce challenge marks affected requests for reauthentication, ignores their
old responses and generates a new ID on the next send. Only the challenged
request becomes immediately due; concurrent deadlines and all transmission
budgets remain intact. Continuous 438 is bounded. Failed encoding or RNG does
not commit a replacement ID or consume a transmission.

No request packet cache was added. See [design-hardening.md](design-hardening.md)
for final CI, fuzz, browser and memory results. External TURN was **not rerun**
for this revision; the network evidence below is historical.

## Historical review findings (F1–F5)

| ID  | Risk    | Issue                                                                                                | Fix |
|-----|---------|------------------------------------------------------------------------------------------------------|-----|
| F1  | Medium  | `turn_handle_response()` skipped txid validation on `STUN_CREATE_PERMISSION_RESPONSE`. A spoofed response could overwrite the client's NONCE via the 438 path or reach `permission_created` log without authorization. | Track per-permission txid in `permissions[].txid`, validate on response receipt. Mirrors the pattern already used for ChannelBind responses. |
| F2  | Low     | `turn_channel_bind()` checked `next_channel > 0x7FFF`, allowing channels in the 0x5000–0x7FFE range. RFC 8656 §12 tightened the channel number space to 0x4000–0x4FFE; 0x4FFF is reserved. | Tighten check to `next_channel > 0x4FFE`; return `NANORTC_ERR_BUFFER_TOO_SMALL` when exhausted. (NanoRTC supports 4 channels max so this is defence in depth.) |
| F3  | Low     | No path to send `Refresh` with `LIFETIME=0`. RFC 5766 §7 requires this for graceful deallocation. Falling back to lifetime expiry leaves server-side state for up to 10 minutes. | Add `turn_deallocate()` that generates an authenticated `Refresh` with `LIFETIME=0` and transitions the state machine to `IDLE`. |
| F4  | Doc     | RFCs 5766 / 8656 / 6156 absent from [docs/references/rfc-index.md](../references/rfc-index.md).        | Add the three RFCs to the Core Protocol Stack table with the relevant sections. |
| F5  | Medium  | `nanortc_add_remote_candidate()` parsed only `<addr>` and `<port>` from the SDP candidate string and silently dropped the `typ` attribute, so every remote candidate landed in `remote_candidates[].type = 0` (HOST). This propagated downstream — `ice.selected_type` never reflected RELAY/SRFLX even when the selected pair pointed at one. The bug was masked because no test or production code path checked `remote_candidates[].type`. Discovered while writing the relay-only e2e tests. | Extend the SDP candidate parser to scan for `typ <type>` (host / srflx / prflx / relay) and store the value in `remote_candidates[].type`. RFC 8839 §5.1. |

## Phase 5.2 — Client-side relay data path (Apr 2026)

After the F1–F5 hardening landed, [tests/interop/test_interop_turn_relay.c](../../tests/interop/test_interop_turn_relay.c) covered libdatachannel running relay-only against a host-only nanortc — exercising the *server* side of nanortc's TURN code (Data Indication unwrap + ChannelData receive demux) but **not** the client side: nanortc's own outbound media wrap had never been driven over a real cellular path. Discovered while bringing up a downstream macOS camera SDK example: `viewer connected` fired but no media flowed across cellular networks.

| ID  | Risk     | Issue                                                                                                                                                                                                                              | Fix |
|-----|----------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|-----|
| F6  | Critical | `ice_handle_stun()` set `ice->selected_type = NANORTC_ICE_CAND_HOST` whenever a USE-CANDIDATE Binding Request arrived, regardless of whether it was unwrapped from a TURN Data Indication / ChannelData. The controlled side could not distinguish a directly-arriving check from a relay-tunneled one (both expose the same inner peer `src`), so `selected_type` never became RELAY on relay paths. `rtc_enqueue_transmit()` then sent SRTP/DTLS/RTCP directly to the cellular peer's NAT'd address — silently dropped. | Add a `bool via_turn` parameter to `ice_handle_stun()` and `rtc_process_receive()`. The recursive calls from `turn_unwrap_data` / `turn_unwrap_channel_data` pass `via_turn=true`; the top-level `nanortc_handle_input` entry passes `false`. On USE-CANDIDATE with `via_turn`, `selected_type` becomes `RELAY` so the lazy-wrap decision in `rtc_enqueue_transmit()` fires. RFC 8445 §7.2.1.4. |
| F7  | High     | `rtc_enqueue_transmit()` originally wrapped in TURN eagerly: a burst of N media packets in one tick all wrote into the shared `stun_buf` scratch, leaving N output-queue slots pointing at whatever the *last* writer left behind. N-1 packets were silently corrupted into the last one. Hidden by the previous test profile (1 small DC message at a time).                    | Defer the TURN wrap to `nanortc_poll_output()`. `rtc_enqueue_transmit()` now records `out_wrap_meta[slot].via_turn` + `peer_dest` and stores the unwrapped data; the actual ChannelData / Send-indication encode runs once per `nanortc_poll_output()` call into a dedicated `turn_buf` (sized via `NANORTC_TURN_BUF_SIZE`). Per-output stamping serialises wrap operations across ticks without per-slot scratch buffers. |
| F8  | High     | RFC 7675 consent freshness checks were generated via `rtc_enqueue_output()` directly, bypassing the TURN wrap path. On a RELAY-selected pair, these checks went `sendto()` straight to the NAT'd peer address; the consent timer expired after `NANORTC_ICE_CONSENT_TIMEOUT_MS` and the call dropped, ~30 s after handshake. | Route consent-check output through `rtc_enqueue_transmit()` so the lazy wrap fires when `selected_type == RELAY`. |
| F9  | Medium   | `CreatePermission` was only emitted for `ice.remote_candidates[0]` at the moment TURN reached `ALLOCATED`. With trickle ICE the browser's host candidates usually arrive first and the relay/srflx ones come later, so the working pair often lacked a permission and traffic was silently dropped by the TURN server (RFC 5766 §9). | Move the fan-out into `rtc_process_timers()`: each tick walks `remote_candidates[]`, finds the first peer without an active permission entry, and emits one `CreatePermission` for it. One per tick is required because `turn_buf` is shared scratch; for typical browser candidate counts the fan-out completes in <100 ms. New trickle candidates are picked up on the next tick. |
| F10 | Doc      | No runtime way to tell whether outbound media was actually traversing the relay vs. silently dropping into a direct-send dead end.                                                                                                  | Add `stats_enqueue_via_turn` / `stats_enqueue_direct` / `stats_wrap_dropped` / `stats_tx_queue_full` (`uint32_t`) on `nanortc_t`, incremented inside `rtc_enqueue_transmit()` and `nanortc_poll_output()`. 16 bytes of overhead; reserved for a future `nanortc_get_stats()` public API. |
| F11 | High     | The fixed four-entry permission table could be smaller than the remote candidate table, so early candidates could occupy every slot before a later reachable srflx or relay candidate arrived. Duplicate and unsupported ICE candidates also consumed the bounded remote table unnecessarily. | Validate component/transport/port, deduplicate candidates by endpoint with type promotion, and default `NANORTC_TURN_MAX_PERMISSIONS` to `NANORTC_MAX_ICE_CANDIDATES`. Same-family permissions are scheduled in candidate order, one request per tick. Explicit smaller overrides remain supported; overflow is best-effort and does not freeze ICE. |

### F6/F7 verification — what cellular vs LAN buys

The cellular-phone path is the only one that exercises this end-to-end: same-LAN viewers establish via direct host candidates and never need the relay. The fix was hand-verified by capturing the per-session ICE state at `EV_CONNECTED` from a real cellular phone connecting to a downstream macOS camera SDK example:

```
ICE selected pair type=relay  remote=211.90.236.246:60395
turn_st=ALLOCATED perm=3 chan=0
tx_via_turn=8  tx_direct=0  wrap_drop=0  q_full=0
```

`type=relay` confirms `via_turn` propagated through to `selected_type`; `tx_via_turn=8 / tx_direct=0` confirms every outbound packet at handshake was routed through the lazy wrap. Steady state with the viewer connected for 30 seconds showed no growth in `stats_enqueue_direct`, proving consent freshness was also wrapped (F8).

## Transaction and real-network hardening (Jul 2026)

The TURN client now treats each UDP request as an RFC 8489 transaction rather
than as a one-shot datagram. Allocate/Refresh keep one allocation-wide
transaction, while each permission and channel retains its own transaction ID,
transmission count, and deadline; its exponential RTO is derived from that
count. Retransmission with unchanged authentication reconstructs the same request/ID;
the Sep 2026 convergence above retires IDs when the shared nonce changes. Thus
the state cost stays bounded and no maximum-size packet cache is embedded in
`nanortc_t`. The defaults are configurable through `NANORTC_TURN_RTO_MS` and
`NANORTC_TURN_MAX_TRANSMISSIONS`.

Additional correctness changes in this pass:

- A permission becomes active only after an authenticated success response;
  every permission has an independent four-minute refresh deadline.
- Permission retransmit exhaustion enters a bounded retry delay instead of
  failing the allocation. Peer-specific 403/443 rejections are terminal only
  for that peer in the current ICE generation; 508 uses the maximum TURN RTO
  before retry. Authenticated errors 400/437/441 and other unrecoverable
  responses fail the allocation with a protocol error.
- Unknown or completed response transaction IDs are ignored as late/duplicate
  datagrams, as required by RFC 8489, without mutating live TURN state.
- ChannelBind is initiated after a relayed pair is selected and its permission
  succeeds. Channel identity and lookup include peer address, family, and port.
- Relay allocation success registers an actual local ICE candidate when the
  bounded local candidate table has capacity. Controlling connectivity checks
  and steady-state packets route through TURN when the selected local candidate
  is relay.
- A shared `stun:`/`turn:` IP and port is demultiplexed by STUN method and
  transaction ID, preventing the TURN handler from swallowing srflx replies.
- ICE restart reuses a still-live allocation and local relay candidate, but
  clears peer-specific permissions and channels before the new generation.
- Consent responses must pass transaction-ID, FINGERPRINT, and
  MESSAGE-INTEGRITY verification before extending the consent deadline.

### Coverage status

`tests/interop/test_interop_turn_relay_nanortc.c` (5 cases —
`test_relay_nanortc_handshake`, `_dc_string_bidirectional`,
`_channel_data_burst`, `_large_payload`, `_echo_roundtrip`) covers the
nanortc-as-TURN-client direction end-to-end. Both nanortc and
libdatachannel are forced into `relay_only` mode — every byte on the selected
pair must traverse coturn, without assuming the GitHub runner's public NAT
mapping. This exercises F6 (`via_turn` propagation), F7 (lazy wrap), F8
(consent freshness routing), F9 (per-tick `CreatePermission` fan-out),
and F10 (stats counters).

The protected `External TURN Interop` GitHub Actions workflow is manual-only,
uses one repository-wide concurrency slot, and requires environment approval.
It builds before deriving a 30-minute username/password from the protected
`COTURN_AUTH_SECRET`; test processes receive only the derived credential. Its
first stage requires both srflx and relay discovery from the real STUN/TURN
service before signaling; its second stage forces relay-only
candidate pairs. `NANORTC_INTEROP_NETWORK_REQUIRED=1` makes missing
configuration, an unreachable service, and an unusable loopback topology fail.
Optional local runs return CTest skip code 77. Standalone and pull-request CI
use `-LE network`. The ordinary local-coturn job remains a complementary
server/client smoke test, not evidence for the external nanortc relay-client
topology.

The complementary direction (libdatachannel-as-relay-client, nanortc
receiving) is covered by `test_interop_turn_relay.c` in the same
job-less interop suite.

## Historical verification gap addressed

Before this review, [tests/interop/test_interop_turn.c](../../tests/interop/test_interop_turn.c) ran libdatachannel and nanortc on the same host and let them connect over host candidates. The TURN allocation succeeded but **no relayed datagram was ever sent**. ChannelBind, ChannelData, Send Indication, and Data Indication code paths were not exercised by any end-to-end test.

The new [tests/interop/test_interop_turn_relay.c](../../tests/interop/test_interop_turn_relay.c) closes this gap by setting `rtcConfiguration.iceTransportPolicy = RTC_TRANSPORT_POLICY_RELAY` on the libdatachannel side, forcing the peer to advertise only its relay candidate. NanoRTC then has no choice but to communicate with libdatachannel through the TURN server, exercising the full data path. Each test asserts at completion that nanortc's selected ICE pair type is `NANORTC_ICE_CAND_RELAY`.

## Test mapping

| Test                                              | RFC § covered                  | Layer       |
|---------------------------------------------------|--------------------------------|-------------|
| `test_turn_refresh_selector_waits_for_response` | 8489 §6.2.1, 8656 §8 | Unit |
| `test_turn_untrusted_errors_preserve_transaction` | 8489 §9.2.5 | Unit |
| `test_turn_nonce_rotation_restarts_concurrent_requests` | 8489 §§5, 9.2.5 | Unit |
| `test_turn_repeated_438_is_bounded` | bounded authentication retry policy | Unit |
| `test_turn_channel_number_range`                  | 5766 §11.1, 8656 §12           | Unit        |
| `test_turn_channel_data_padding`                  | 5766 §11.5, 8656 §12.4         | Unit        |
| `test_turn_send_indication_no_integrity`          | 5766 §10.1, 8656 §11.1         | Unit        |
| `test_turn_create_permission_has_integrity`       | 5766 §9, 8656 §10, 8489 §14.5   | Unit        |
| `test_turn_channel_bind_has_integrity`            | 5766 §11.1, 8656 §12.1         | Unit        |
| `test_turn_refresh_zero_lifetime_deallocate`      | 5766 §7, 8656 §8               | Unit        |
| `test_turn_create_permission_foreign_txid_ignored` | 5766 §9, 8489 §§5, 6.3          | Unit        |
| `test_turn_permission_508_defers_retry`          | 8656 §10.2, §19             | Unit        |
| `test_turn_permission_unrecoverable_error_fails_allocation` | 8656 §10.2, §19 | Unit |
| `test_e2e_turn_permission_capacity_covers_remote_candidates` | dense trickle ICE | End-to-end |
| `test_e2e_turn_permission_table_full_does_not_freeze_ice` | constrained override | End-to-end |
| `test_turn_message_integrity_hmac_vector`         | 8489 §14.5, §9.2.2             | Unit        |
| `test_relay_only_handshake`                       | end-to-end relay path          | Interop     |
| `test_relay_only_dc_string_bidirectional`         | ChannelData, Data Indication   | Interop     |
| `test_relay_only_channel_data_burst`              | ChannelData under load         | Interop     |
| `test_relay_only_large_payload`                   | ChannelData boundary           | Interop     |
| `test_relay_only_echo_roundtrip`                  | bidirectional relay            | Interop     |
| `test_relay_only_explicit_dealloc`                | 5766 §7 LIFETIME=0             | Interop     |

## How to run

```bash
# Unit tests (no network)
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DNANORTC_FEATURE_TURN=ON
cmake --build build -j
ctest --test-dir build -R test_turn --output-on-failure

# Local coturn for relay e2e
./scripts/start-test-turn.sh
cmake -B build-interop -DNANORTC_BUILD_INTEROP_TESTS=ON -DNANORTC_CRYPTO=openssl
cmake --build build-interop -j
ctest --test-dir build-interop -L turn-relay --output-on-failure
./scripts/stop-test-turn.sh

# External TURN server override
NANORTC_TURN_URL="turn:example.org:3478" \
NANORTC_TURN_USER="alice" \
NANORTC_TURN_PASS="secret" \
  ctest --test-dir build-interop -L turn-relay --output-on-failure
```
