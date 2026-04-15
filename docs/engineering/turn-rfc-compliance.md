# TURN RFC Compliance Review

This document audits the NanoRTC TURN client implementation ([src/nano_turn.c](../../src/nano_turn.c), [src/nano_turn.h](../../src/nano_turn.h)) against:

- **RFC 5766** — Traversal Using Relays around NAT (TURN)
- **RFC 8656** — Traversal Using Relays around NAT (TURN-bis), updates 5766
- **RFC 8489** — Session Traversal Utilities for NAT (STUN), the TURN authentication base
- **RFC 7983** — Multiplexing Scheme Updates for DTLS-SRTP (ChannelData demux)

NanoRTC targets the WebRTC-relevant TURN subset: UDP transport, long-term credential authentication, ChannelBind for low-overhead relaying. Optional TURN features (TCP/TLS transport, EVEN-PORT, address-family negotiation) are out of scope.

## Compliance Matrix

### Allocation

| RFC §                      | Requirement                                                        | Status | Implementation |
|----------------------------|--------------------------------------------------------------------|--------|----------------|
| 5766 §6 / 8656 §7          | Allocate Request: STUN method 0x003                                | OK     | [nano_turn.c:218](../../src/nano_turn.c) |
| 5766 §6 / 8656 §7          | Allocate carries REQUESTED-TRANSPORT (UDP=17)                      | OK     | [nano_turn.c:221-223](../../src/nano_turn.c) |
| 5766 §6 / 8656 §7          | Allocate carries LIFETIME (default 600 s)                          | OK     | [nano_turn.c:225-228](../../src/nano_turn.c) |
| 5766 §6.2 / 8656 §7.2      | First Allocate is unauthenticated                                  | OK     | [nano_turn.c:240-244](../../src/nano_turn.c) |
| 5766 §6.2 / 8656 §7.2      | On 401: extract REALM + NONCE, derive long-term key, retry         | OK     | [nano_turn.c:293-323](../../src/nano_turn.c) |
| 5766 §6.3 / 8656 §7.3      | Authenticated Allocate adds USERNAME / REALM / NONCE / MI          | OK     | [nano_turn.c:230-238](../../src/nano_turn.c) |
| 5766 §6.3 / 8656 §7.3      | Extract XOR-RELAYED-ADDRESS from success response                  | OK     | [nano_turn.c:276-290](../../src/nano_turn.c) |
| 5766 §6.3 / 8656 §7.3      | Extract LIFETIME from success response                             | OK     | [nano_turn.c:286](../../src/nano_turn.c) |

### Refresh

| RFC §                | Requirement                                            | Status | Implementation |
|----------------------|--------------------------------------------------------|--------|----------------|
| 5766 §7 / 8656 §6    | Refresh Request: STUN method 0x004                     | OK     | [nano_turn.c:439](../../src/nano_turn.c) |
| 5766 §7 / 8656 §6    | Refresh authenticated (USERNAME/REALM/NONCE/MI)        | OK     | [nano_turn.c:447-454](../../src/nano_turn.c) |
| 5766 §7 / 8656 §6    | Update lifetime from success response                  | OK     | [nano_turn.c:347-350](../../src/nano_turn.c) |
| 5766 §7 / 8656 §6    | LIFETIME=0 ⇒ explicit deallocation                     | OK     | [nano_turn.c](../../src/nano_turn.c) — added by F3 |
| 5766 §7 / 8656 §6    | Pre-emptive refresh before lifetime expiry             | OK     | [nano_turn.c:458-460](../../src/nano_turn.c) |
| 5766 §7 / 8489 §9.2  | Handle 438 Stale Nonce, refresh nonce, retry           | OK     | [nano_turn.c:325-339, 352-362](../../src/nano_turn.c) |

### CreatePermission

| RFC §                | Requirement                                            | Status | Implementation |
|----------------------|--------------------------------------------------------|--------|----------------|
| 5766 §9 / 8656 §9    | CreatePermission method 0x008                          | OK     | [nano_turn.c:488](../../src/nano_turn.c) |
| 5766 §9 / 8656 §9    | Carries XOR-PEER-ADDRESS                               | OK     | [nano_turn.c:491-493](../../src/nano_turn.c) |
| 5766 §9 / 8656 §9    | Authenticated (USERNAME/REALM/NONCE/MI)                | OK     | [nano_turn.c:495-502](../../src/nano_turn.c) |
| 5766 §9 / 8656 §9    | Permission lifetime 5 min, refresh @ 4 min             | OK     | [nano_turn.c:801-837](../../src/nano_turn.c) |
| 5766 §9 / 8656 §9    | Per-permission txid tracked & validated on response    | OK     | [nano_turn.c](../../src/nano_turn.c) — added by F1 |

### Send / Data Indications

| RFC §                | Requirement                                            | Status | Implementation |
|----------------------|--------------------------------------------------------|--------|----------------|
| 5766 §10 / 8656 §10  | Send Indication method 0x016                           | OK     | [nano_turn.c:553](../../src/nano_turn.c) |
| 5766 §10 / 8656 §10  | Send Indication: XOR-PEER-ADDRESS + DATA only          | OK     | [nano_turn.c:557-561](../../src/nano_turn.c) |
| 5766 §10.1 / 8656 §10.1 | Send Indication MUST NOT carry MESSAGE-INTEGRITY    | OK     | [nano_turn.c:534-565](../../src/nano_turn.c) (no MI write) |
| 5766 §10.2 / 8656 §10.2 | Data Indication unwrap: peer addr + payload         | OK     | [nano_turn.c:568-595](../../src/nano_turn.c) |

### ChannelBind / ChannelData

| RFC §                | Requirement                                            | Status | Implementation |
|----------------------|--------------------------------------------------------|--------|----------------|
| 5766 §11 / 8656 §11  | ChannelBind method 0x009                               | OK     | [nano_turn.c:670](../../src/nano_turn.c) |
| 5766 §11 / 8656 §12  | Channel number range 0x4000–0x4FFE (8656 narrowing)    | OK     | [nano_turn.c](../../src/nano_turn.c) — tightened by F2 |
| 5766 §11 / 8656 §11  | CHANNEL-NUMBER attribute (4 bytes: 2B channel + 2B RFFU) | OK   | [nano_turn.c:673-678](../../src/nano_turn.c) |
| 5766 §11 / 8656 §11  | XOR-PEER-ADDRESS                                       | OK     | [nano_turn.c:681-682](../../src/nano_turn.c) |
| 5766 §11 / 8656 §11  | Authenticated (USERNAME/REALM/NONCE/MI)                | OK     | [nano_turn.c:684-691](../../src/nano_turn.c) |
| 5766 §11 / 8656 §11  | Per-channel txid tracked, response matched by txid     | OK     | [nano_turn.c:386-393, 664](../../src/nano_turn.c) |
| 5766 §11 / 8656 §11  | Channel binding lifetime 10 min, refresh @ 9 min       | OK     | [nano_turn.c:839-872](../../src/nano_turn.c) |
| 5766 §11.4 / 8656 §12.4 | ChannelData framing: 4-byte header + payload + 4-byte align padding | OK | [nano_turn.c:698-745](../../src/nano_turn.c) |

### STUN base / Long-term credentials

| RFC §                | Requirement                                            | Status | Implementation |
|----------------------|--------------------------------------------------------|--------|----------------|
| 8489 §9.2.2          | key = MD5(username ":" realm ":" password)             | OK     | [nano_turn.c:125-151](../../src/nano_turn.c) |
| 8489 §14.5           | MESSAGE-INTEGRITY = HMAC-SHA1(key, message)            | OK     | [nano_turn.c:97-118](../../src/nano_turn.c) |
| 8489 §14.5           | MESSAGE-INTEGRITY computed over header with adjusted length field | OK | [nano_turn.c:103-116](../../src/nano_turn.c) |

### Demultiplexing

| RFC §        | Requirement                                                                | Status | Implementation |
|--------------|----------------------------------------------------------------------------|--------|----------------|
| 7983 §3      | First-byte demux: STUN [0x00–0x03], ChannelData [0x40–0x7F], DTLS [0x14–0x3F], RTP [0x80–0xBF] | OK | [nano_rtc.c:919-944](../../src/nano_rtc.c), [nano_turn.c:206](../../src/nano_turn.c) |
| 7983 §3      | ChannelData detection by first nibble (0x4 / 0x7)                          | OK     | [nano_turn.c:206](../../src/nano_turn.c) |

## Out-of-scope features (deliberate non-implementation)

These RFC features are not relevant to the WebRTC TURN profile NanoRTC targets. They are documented here so readers know they are intentional gaps, not oversights.

| RFC §                          | Feature                                                                | Why out of scope |
|--------------------------------|------------------------------------------------------------------------|------------------|
| 5766 §14.6 / 8656 §14.5        | EVEN-PORT (request even-numbered relay port)                           | RTP-pair allocation; WebRTC does not require it |
| 5766 §14.9 / 8656 §14.7        | RESERVATION-TOKEN (reserve port pair across two allocations)           | Used with EVEN-PORT |
| 8656 §14.7                     | REQUESTED-ADDRESS-FAMILY (choose IPv4 vs IPv6 relay)                   | NanoRTC currently allocates IPv4 only |
| 8656 §14.8                     | ADDITIONAL-ADDRESS-FAMILY (dual-stack relay)                           | Same as above |
| 5766 §14.8                     | DONT-FRAGMENT (set IP DF bit on relayed datagrams)                     | Optional; UDP fragmentation generally tolerated |
| RFC 6062                       | TURN extensions for TCP allocations (server↔peer over TCP)             | NanoRTC peer leg is UDP only |
| RFC 6156                       | TURN over IPv6 (client↔server leg over IPv6)                           | Possible future work, see also IPv6 feature flag |
| RFC 7635                       | TURN OAuth third-party auth                                            | Long-term credentials cover the common case |
| RFC 8489 §6.2.2                | TLS / DTLS transport for client↔server (TURN-S)                        | Possible future work for hostile-network operation |

## Review findings (fixed in this hardening pass)

| ID  | Risk    | Issue                                                                                                | Fix |
|-----|---------|------------------------------------------------------------------------------------------------------|-----|
| F1  | Medium  | `turn_handle_response()` skipped txid validation on `STUN_CREATE_PERMISSION_RESPONSE`. A spoofed response could overwrite the client's NONCE via the 438 path or reach `permission_created` log without authorization. | Track per-permission txid in `permissions[].txid`, validate on response receipt. Mirrors the pattern already used for ChannelBind responses. |
| F2  | Low     | `turn_channel_bind()` checked `next_channel > 0x7FFF`, allowing channels in the 0x5000–0x7FFE range. RFC 8656 §12 tightened the channel number space to 0x4000–0x4FFE; 0x4FFF is reserved. | Tighten check to `next_channel > 0x4FFE`; return `NANORTC_ERR_BUFFER_TOO_SMALL` when exhausted. (NanoRTC supports 4 channels max so this is defence in depth.) |
| F3  | Low     | No path to send `Refresh` with `LIFETIME=0`. RFC 5766 §7 requires this for graceful deallocation. Falling back to lifetime expiry leaves server-side state for up to 10 minutes. | Add `turn_deallocate()` that generates an authenticated `Refresh` with `LIFETIME=0` and transitions the state machine to `IDLE`. |
| F4  | Doc     | RFCs 5766 / 8656 / 6156 absent from [docs/references/rfc-index.md](../references/rfc-index.md).        | Add the three RFCs to the Core Protocol Stack table with the relevant sections. |
| F5  | Medium  | `nanortc_add_remote_candidate()` parsed only `<addr>` and `<port>` from the SDP candidate string and silently dropped the `typ` attribute, so every remote candidate landed in `remote_candidates[].type = 0` (HOST). This propagated downstream — `ice.selected_type` never reflected RELAY/SRFLX even when the selected pair pointed at one. The bug was masked because no test or production code path checked `remote_candidates[].type`. Discovered while writing the relay-only e2e tests. | Extend the SDP candidate parser to scan for `typ <type>` (host / srflx / prflx / relay) and store the value in `remote_candidates[].type`. RFC 8839 §5.1. |

## Verification gap addressed

Before this review, [tests/interop/test_interop_turn.c](../../tests/interop/test_interop_turn.c) ran libdatachannel and nanortc on the same host and let them connect over host candidates. The TURN allocation succeeded but **no relayed datagram was ever sent**. ChannelBind, ChannelData, Send Indication, and Data Indication code paths were not exercised by any end-to-end test.

The new [tests/interop/test_interop_turn_relay.c](../../tests/interop/test_interop_turn_relay.c) closes this gap by setting `rtcConfiguration.iceTransportPolicy = RTC_TRANSPORT_POLICY_RELAY` on the libdatachannel side, forcing the peer to advertise only its relay candidate. NanoRTC then has no choice but to communicate with libdatachannel through the TURN server, exercising the full data path. Each test asserts at completion that nanortc's selected ICE pair type is `NANORTC_ICE_CAND_RELAY`.

## Test mapping

| Test                                              | RFC § covered                  | Layer       |
|---------------------------------------------------|--------------------------------|-------------|
| `test_turn_channel_number_range`                  | 5766 §11.1, 8656 §12           | Unit        |
| `test_turn_channel_data_padding`                  | 5766 §11.5, 8656 §12.4         | Unit        |
| `test_turn_send_indication_no_integrity`          | 5766 §10.1, 8656 §10.1         | Unit        |
| `test_turn_create_permission_has_integrity`       | 5766 §9, 8656 §9, 8489 §14.5   | Unit        |
| `test_turn_channel_bind_has_integrity`            | 5766 §11.2, 8656 §11.2         | Unit        |
| `test_turn_refresh_zero_lifetime_deallocate`      | 5766 §7, 8656 §6               | Unit        |
| `test_turn_create_permission_txid_validation`     | 5766 §9, 8489 §6.3.1           | Unit        |
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
