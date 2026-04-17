# RFC Compliance Checklist

Per-module audit of NanoRTC against authoritative RFC specifications.

**Legend:**
- Impl: Implementation status (Y = yes, P = partial, N = no, N/A = not applicable for embedded target)
- Test: Test coverage (Y = tested, P = partial, N = no test)
- Gap: Known gap description

---

## RFC 8489 — STUN (Session Traversal Utilities for NAT)

**File:** `src/nano_stun.c` | **Tests:** `tests/test_stun.c` (40 tests)

### Message Structure (RFC 8489 §5-6)

| # | Requirement | RFC | Impl | Test | Gap |
|---|-------------|-----|------|------|-----|
| 1 | First two bits MUST be zero | §6 | Y | Y | `test_stun_parse_top_bits_set` |
| 2 | Magic cookie MUST be 0x2112A442 | §6 | Y | Y | `test_stun_parse_bad_magic_cookie` |
| 3 | Message length MUST be multiple of 4 | §6 | Y | Y | `test_stun_parse_length_not_aligned` |
| 4 | Transaction ID is 96-bit (12 bytes) | §6 | Y | Y | Parsed in all vectors |
| 5 | Header is exactly 20 bytes | §6 | Y | Y | `test_stun_header_size` |
| 6 | Message types: Request/Response/Error/Indication | §6 | Y | Y | All four parsed |

### Attributes (RFC 8489 §14)

| # | Requirement | RFC | Impl | Test | Gap |
|---|-------------|-----|------|------|-----|
| 7 | USERNAME (0x0006) | §14.3 | Y | Y | With padding |
| 8 | MESSAGE-INTEGRITY HMAC-SHA1 | §14.5 | Y | Y | RFC 5769 vectors + roundtrip |
| 9 | FINGERPRINT CRC-32 XOR 0x5354554E | §14.7 | Y | Y | RFC 5769 vectors |
| 10 | XOR-MAPPED-ADDRESS IPv4 | §14.2 | Y | Y | RFC 5769 §2.2 |
| 11 | XOR-MAPPED-ADDRESS IPv6 | §14.2 | Y | Y | RFC 5769 §2.3 |
| 12 | ERROR-CODE attribute (0x0009) | §14.8 | Y | P | Parsed but encode/decode test missing |
| 13 | PRIORITY (ICE, 0x0024) | RFC 8445 | Y | Y | `test_stun_parse_priority_and_use_candidate` |
| 14 | USE-CANDIDATE (ICE, 0x0025) | RFC 8445 | Y | Y | Same test |
| 15 | ICE-CONTROLLING / ICE-CONTROLLED | RFC 8445 | Y | Y | Encoder roundtrips |
| 16 | SOFTWARE attribute (0x8022) | §14.10 | Y | Y | Parsed in str0m vector |
| 17 | Unknown comprehension-required attrs → reject | §7.3.1 | P | N | Not explicitly tested |
| 18 | Unknown comprehension-optional attrs → ignore | §7.3.1 | Y | P | str0m vector has NETWORK-COST |
| 19 | REALM / NONCE (digest auth) | §14.3/§14.4 | N | N | N/A for ICE use case |
| 20 | ALTERNATE-SERVER | §14.11 | N | N | N/A for ICE use case |

### Integrity & Security (RFC 8489 §14.5, §14.7)

| # | Requirement | RFC | Impl | Test | Gap |
|---|-------------|-----|------|------|-----|
| 21 | MI MUST appear before FINGERPRINT | §14.5 | Y | Y | `test_stun_attrs_after_mi_ignored` |
| 22 | FINGERPRINT MUST be last attribute | §14.7 | Y | Y | `test_stun_nothing_after_fingerprint` |
| 23 | Wrong password → integrity fail | §14.5 | Y | Y | `test_rfc5769_request_wrong_password` |
| 24 | Corrupted data → integrity fail | §14.5 | Y | Y | `test_stun_verify_integrity_corrupted_data` |

### Test Vectors (RFC 5769)

| # | Vector | Impl | Test |
|---|--------|------|------|
| 25 | §2.1 Sample Request | Y | Y |
| 26 | §2.2 IPv4 Response | Y | Y |
| 27 | §2.3 IPv6 Response | Y | Y |

---

## RFC 8445 — ICE (Interactive Connectivity Establishment)

**File:** `src/nano_ice.c` | **Tests:** `tests/test_ice.c` (24 tests) | **Full audit:** [engineering/ice-rfc-compliance.md](engineering/ice-rfc-compliance.md)

**Design principle:** interop-first against Chrome / Firefox / libdatachannel under embedded memory ceilings. Features that no mainstream WebRTC peer exercises (prflx materialisation, 487 Role Conflict auto-swap, per-pair state machine, triggered-check queue) are deferred with rationale in the module-level audit; everything we do implement is on-spec and interop-verified.

### Candidate Types (RFC 8445 §5.1.1)

| # | Requirement | RFC | Impl | Test | Gap |
|---|-------------|-----|------|------|-----|
| 1 | Host candidates | §5.1.1 | Y | Y | Default candidate type |
| 1b | IPv6 host candidates | §5.1.1 | Y | Y | `test_e2e_ipv6_remote_candidate`; guarded by `NANORTC_FEATURE_IPV6` |
| 2 | Server Reflexive (SRFLX) | §5.1.1 | N | N | Requires STUN server — N/A embedded |
| 3 | Peer Reflexive (PRFLX) | §5.1.1 | N | N | N/A embedded |
| 4 | Relay (TURN) | §5.1.1 | N | N | N/A embedded (RFC 5766) |

### Priority Calculation (RFC 8445 §5.1.2.1)

| # | Requirement | RFC | Impl | Test | Gap |
|---|-------------|-----|------|------|-----|
| 5 | priority = (2^24)*type + (2^8)*local + (256-comp) | §5.1.2.1 | Y | N | Formula implemented but no value verification test |

### Pair Formation (RFC 8445 §6.1.2.2)

| # | Requirement | RFC | Impl | Test | Gap |
|---|-------------|-----|------|------|-----|
| 5a | Same-address-family pairs only (MUST) | §6.1.2.2 | Y | Y | `test_ice_pair_family_filter_*` — cross-family pairs are skipped before a pending slot is consumed; v4+v6 dual-stack covered by `test_e2e_ipv6_loopback_connects` |
| 5b | Tie-breaker is a random 64-bit value (§5.2)             | §5.2 | Y | Y | `test_e2e_tie_breaker_is_randomised` — filled via `cfg->crypto->random_bytes()` in `nanortc_init()` |
| 5c | SDP `a=candidate` priority matches STUN PRIORITY attribute | §5.1.2.1 | Y | Y | SDP srflx/relay priorities now emitted via `ICE_SRFLX_PRIORITY` / `ICE_RELAY_PRIORITY` with the runtime-matching idx |

### Connectivity Checks (RFC 8445 §7)

| # | Requirement | RFC | Impl | Test | Gap |
|---|-------------|-----|------|------|-----|
| 6 | Controlling: generate STUN Binding Requests | §7.1.1 | Y | Y | `test_ice_generate_check_basic` |
| 7 | Check pacing (Ta interval) | §7.1.1 | Y | Y | `test_ice_generate_check_pacing` |
| 8 | Controlled: respond to requests | §7.2.1 | Y | Y | `test_ice_controlled_handle_request` |
| 9 | USERNAME = remote_ufrag:local_ufrag | §7.2.1.1 | Y | Y | `test_ice_reject_bad_username` |
| 10 | MESSAGE-INTEGRITY with local_pwd | §7.2.1.1 | Y | Y | `test_ice_reject_bad_password` |
| 11 | USE-CANDIDATE → nomination | §7.2.1.4 | Y | Y | `test_ice_use_candidate_nominates` |
| 12 | No USE-CANDIDATE → no nomination | §7.2.1.4 | Y | Y | `test_ice_no_use_candidate_no_nomination` |
| 13 | Process binding response | §7.3 | Y | Y | `test_ice_controlling_receives_response` |
| 14 | Reject wrong transaction ID | §7.3 | Y | Y | `test_ice_controlling_rejects_wrong_txid` |
| 14a | Incoming Binding Request MUST carry MESSAGE-INTEGRITY + FINGERPRINT | §7.1.2.1, §8.1 | Y | Y | `test_ice_request_without_fingerprint_rejected`; absent MI or FP on request/response is a protocol error |
| 14b | Incoming Binding Response MUST carry MESSAGE-INTEGRITY + FINGERPRINT | §7.1.3 | Y | Y | `test_ice_response_without_integrity_rejected` |
| 14c | Binding Error Response (0x0111) frees pending slot | §7.3.1.1 + RFC 8489 §6.3.4 | Y | Y | `test_ice_binding_error_frees_pending_slot`; full 487 auto-swap deferred (see ice-rfc-compliance.md) |
| 15 | Role conflict resolution (auto swap) | §7.3.1.1 | N | N | Deferred — not observed in WebRTC peer behavior (browsers negotiate role at offer/answer) |
| 16 | Multiple remote candidates cycling | — | Y | N | Not explicitly tested |

### State Machine (RFC 8445 §8)

| # | Requirement | RFC | Impl | Test | Gap |
|---|-------------|-----|------|------|-----|
| 17 | NEW → CHECKING | §8 | Y | Y | `test_ice_state_new_to_checking` |
| 18 | CHECKING → CONNECTED | §8 | Y | Y | `test_ice_state_checking_to_connected` |
| 19 | CHECKING → FAILED (max checks) | §8 | Y | Y | `test_ice_state_checking_to_failed` |
| 20 | No checks after CONNECTED | §8 | Y | Y | `test_ice_no_checks_after_connected` |
| 21 | No checks after DISCONNECTED (consent lost) | RFC 7675 §5.2 | Y | Y | `test_ice_generate_check_noop_in_disconnected` — DISCONNECTED requires `ice_restart`, not more checks |
| 22 | Unarmed consent_expiry_ms in CONNECTED surfaces as expired | RFC 7675 §5.1 | Y | Y | `test_consent_expired_when_unarmed` — fails loud instead of silently disabling the timeout |

---

## RFC 6347 — DTLS 1.2 / RFC 5764 — DTLS-SRTP

**File:** `src/nano_dtls.c` | **Tests:** `tests/test_dtls.c` (9 tests)

| # | Requirement | RFC | Impl | Test | Gap |
|---|-------------|-----|------|------|-----|
| 1 | DTLS 1.2 handshake (ClientHello → Finished) | RFC 6347 §4.2 | Y | Y | `test_dtls_handshake_loopback` |
| 2 | Record layer content types (20-25) | RFC 6347 §4.1 | Y | N | No explicit content type verification test |
| 3 | Epoch management | RFC 6347 §4.1 | Y | N | No epoch increment verification test |
| 4 | Handshake timeout & retransmit | RFC 6347 §4.2.4 | P | N | Timer doubling not explicitly tested |
| 5 | Certificate fingerprint (SHA-256) | RFC 5764 | Y | Y | `test_dtls_fingerprint_format/unique` |
| 6 | Keying material export (60 bytes) | RFC 5764 §4.2 | Y | Y | `test_dtls_keying_material` |
| 7 | Client/server key split | RFC 5764 §4.2 | Y | Y | Via SRTP key direction test |
| 8 | Encrypt/decrypt after handshake | RFC 6347 §4.1 | Y | Y | `test_dtls_encrypt_decrypt` |
| 9 | State machine (INIT→HANDSHAKING→ESTABLISHED→CLOSED) | — | Y | Y | `test_dtls_wrong_state` |
| 10 | close_notify on shutdown | RFC 6347 §4.1.2.1 | Y | Y | `test_dtls_close_notify` — both OpenSSL and mbedTLS backends |

---

## RFC 4960 — SCTP / RFC 3758 — PR-SCTP

**File:** `src/nano_sctp.c` | **Tests:** `tests/test_sctp.c` (36 tests)

### Packet Format (RFC 4960 §3)

| # | Requirement | RFC | Impl | Test | Gap |
|---|-------------|-----|------|------|-----|
| 1 | Common header (12 bytes): src/dst port, vtag, checksum | §3.1 | Y | Y | `test_parse_header_basic` |
| 2 | CRC-32c checksum | §3.1 | Y | Y | `test_checksum_roundtrip/corruption` |
| 3 | Chunk type/flags/length format | §3.2 | Y | Y | All chunk encode/parse tests |
| 4 | Chunk padding to 4-byte boundary | §3.2 | Y | Y | `test_data_chunk_padding` |

### Chunk Types (RFC 4960 §3.3)

| # | Chunk | RFC | Impl | Test | Gap |
|---|-------|-----|------|------|-----|
| 5 | INIT (type 1) | §3.3.2 | Y | Y | `test_encode_parse_init_roundtrip` |
| 6 | INIT-ACK (type 2) | §3.3.3 | Y | Y | `test_encode_parse_init_ack_with_cookie` |
| 7 | COOKIE-ECHO (type 10) | §3.3.11 | Y | Y | `test_encode_cookie_echo_roundtrip` |
| 8 | COOKIE-ACK (type 11) | §3.3.12 | Y | Y | `test_encode_cookie_ack` |
| 9 | DATA (type 0) with B/E/U flags | §3.3.1 | Y | Y | `test_encode_parse_data_roundtrip` |
| 10 | SACK (type 3) | §3.3.4 | Y | Y | `test_encode_parse_sack_roundtrip` |
| 11 | HEARTBEAT (type 4) | §3.3.5 | Y | Y | `test_encode_heartbeat_roundtrip` |
| 12 | HEARTBEAT-ACK (type 5) | §3.3.6 | Y | Y | `test_encode_heartbeat_ack` |
| 13 | ABORT (type 6) | §3.3.7 | Y | Y | `test_handle_data_abort` |
| 14 | SHUTDOWN (type 7) | §3.3.8 | Y | Y | `test_encode_shutdown` |
| 15 | ERROR (type 9) | §3.3.10 | P | N | Defined but not generated/tested |
| 16 | FORWARD-TSN (RFC 3758) | §3 | Y | Y | `test_encode_forward_tsn` |

### Association Setup (RFC 4960 §5)

| # | Requirement | RFC | Impl | Test | Gap |
|---|-------------|-----|------|------|-----|
| 17 | 4-way handshake (INIT→INIT-ACK→COOKIE-ECHO→COOKIE-ACK) | §5.1 | Y | Y | `test_two_instance_handshake_server_client` |
| 18 | INIT vtag MUST be 0 | §8.5.1 | Y | N | Not explicitly verified in tests |
| 19 | a_rwnd >= 1500 in INIT | §5.1 | Y | N | Not validated in tests |
| 20 | Cookie HMAC verification | §5.1.5 | Y | Y | Via handshake test |

### Data Transfer (RFC 4960 §6)

| # | Requirement | RFC | Impl | Test | Gap |
|---|-------------|-----|------|------|-----|
| 21 | TSN tracking (cumulative) | §6.2.1 | Y | Y | `test_two_instance_data_exchange` |
| 22 | Gap ack blocks for out-of-order | §6.2.1 | Y | Y | `test_sctp_gap_*` — reorder buffer + gap ack blocks + delivery queue |
| 23 | SACK generation | §6.2 | Y | Y | `test_sack_drains_send_queue` |
| 24 | Retransmission on timeout | §6.3 | Y | Y | Via RTO mechanism |
| 25 | Selective ACK gap block encoding | §3.3.4 | Y | Y | `test_sctp_sack_with_gaps` — `nsctp_encode_sack_with_gaps()` |
| 26 | Stream Sequence Numbers (SSN) | §6.6 | P | P | Only with DC_ORDERED feature |

### Fault Management (RFC 4960 §8)

| # | Requirement | RFC | Impl | Test | Gap |
|---|-------------|-----|------|------|-----|
| 27 | SHUTDOWN sequence | §9.2 | P | N | Implemented but no full sequence test |
| 28 | ABORT handling | §9.1 | Y | Y | `test_handle_data_abort` |
| 29 | Max retransmit → fail | §8.1 | Y | P | Configurable but not edge-tested |

### RFC 4960 Appendix Test Vectors

| # | Vector | Test | Gap |
|---|--------|------|-----|
| 30 | INIT chunk byte-level | N | No hardcoded RFC vectors |
| 31 | DATA chunk byte-level | N | No hardcoded RFC vectors |
| 32 | SACK chunk byte-level | N | No hardcoded RFC vectors |

---

## RFC 8832 — DCEP / RFC 8831 — DataChannel

**File:** `src/nano_datachannel.c` | **Tests:** via `test_sctp.c` + `test_e2e.c`

| # | Requirement | RFC | Impl | Test | Gap |
|---|-------------|-----|------|------|-----|
| 1 | DATA_CHANNEL_OPEN (0x03) | RFC 8832 §5 | Y | Y | Via SCTP DCEP tests |
| 2 | DATA_CHANNEL_ACK (0x02) | RFC 8832 §6 | Y | Y | Via SCTP DCEP tests |
| 3 | Channel type field | RFC 8832 §5 | Y | Y | Reliable/unreliable |
| 4 | Label and protocol strings | RFC 8832 §5 | Y | Y | `test_e2e_get_datachannel_label` |
| 5 | PPID 50 = WebRTC String | RFC 8831 §6.1 | Y | Y | Via DCEP/SCTP |
| 6 | PPID 51 = WebRTC String Empty | RFC 8831 §6.1 | Y | P | Defined but not tested |
| 7 | PPID 52 = WebRTC Binary | RFC 8831 §6.1 | Y | P | Defined but not tested |
| 8 | PPID 53 = WebRTC Binary Empty | RFC 8831 §6.1 | Y | P | Defined but not tested |
| 9 | Idempotent OPEN (re-ACK on retransmit) | RFC 8832 §5 | Y | Y | Per quality notes |
| 10 | Bidirectional message exchange | RFC 8831 | Y | N | No e2e message exchange test |

---

## RFC 8866 — SDP / RFC 8829 — JSEP

**File:** `src/nano_sdp.c` | **Tests:** `tests/test_sdp.c` (21 tests)

| # | Requirement | RFC | Impl | Test | Gap |
|---|-------------|-----|------|------|-----|
| 1 | v=0 version line | RFC 8866 §5.1 | Y | P | Parsed but no explicit reject-on-missing test |
| 2 | o= origin line | RFC 8866 §5.2 | Y | Y | Generated/parsed; IPv6-aware (`IN IP6 ::` when local candidate is IPv6) |
| 2b | c= connection line IPv4/IPv6 | RFC 8866 §5.7 | Y | Y | `test_sdp_generate_ipv6/ipv4_connection_line`; auto-selects `IN IP4`/`IN IP6` |
| 3 | m= media description | RFC 8866 §5.14 | Y | Y | DC/audio/video media lines |
| 4 | a=ice-ufrag / a=ice-pwd | RFC 8839 | Y | Y | `test_sdp_parse_chrome_offer` |
| 5 | a=fingerprint (SHA-256) | RFC 8122 | Y | Y | Parsed and generated |
| 6 | a=setup (active/passive/actpass) | RFC 4145 | Y | Y | 3 setup role tests |
| 7 | a=group:BUNDLE | RFC 8829 §5.3.1 | Y | P | Parsed but no multi-mid validation test |
| 8 | a=mid | RFC 8829 §5.3.1 | Y | Y | Preserved in offer/answer |
| 9 | a=candidate | RFC 8839 | Y | Y | `test_sdp_parse_no_candidates` |
| 10 | a=rtcp-mux | RFC 5761 | Y | Y | Generated |
| 11 | a=ice-lite | RFC 8445 §2.2 | P | N | Not explicitly parsed/tested |
| 12 | Malformed SDP rejection | — | P | P | `test_sdp_parse_missing_ufrag` but not comprehensive |
| 13 | SCTP port (m=application ... DTLS/SCTP) | RFC 8841 | Y | Y | Parsed/generated |
| 14 | Multi-browser compat (Chrome/Firefox/Safari) | — | Y | Y | 4 browser offer tests |
| 15 | Direction complement (RFC 3264 §6) | RFC 3264 | Y | Y | In SDP generation |

---

## RFC 3550 — RTP / RTCP

### RTP (RFC 3550 §5)

**File:** `src/nano_rtp.c` | **Tests:** `tests/test_rtp.c` (8 tests)

| # | Requirement | RFC | Impl | Test | Gap |
|---|-------------|-----|------|------|-----|
| 1 | V=2 version | §5.1 | Y | Y | `test_rtp_unpack_bad_version` |
| 2 | Payload type (7 bits) | §5.1 | Y | Y | All pack/unpack tests |
| 3 | Sequence number (16 bits) | §5.1 | Y | Y | `test_rtp_pack_basic` |
| 4 | Timestamp (32 bits) | §5.1 | Y | Y | Packed/unpacked |
| 5 | SSRC (32 bits) | §5.1 | Y | Y | Packed/unpacked |
| 6 | Marker bit | §5.1 | Y | Y | `test_rtp_pack/unpack_marker` |
| 7 | Padding (P bit) | §5.1 | Y | N | Implemented but not tested |
| 8 | Extension header (X bit) | §5.3.1 | Y | N | Parsed but not tested |
| 9 | CSRC list (CC > 0) | §5.1 | Y | N | Skipped but not tested |
| 10 | 12-byte fixed header | §5.1 | Y | Y | `test_rtp_header_size` |

### RTCP (RFC 3550 §6 + RFC 4585)

**File:** `src/nano_rtcp.c` | **Tests:** `tests/test_rtcp.c` (19 tests)

| # | Requirement | RFC | Impl | Test | Gap |
|---|-------------|-----|------|------|-----|
| 11 | Sender Report (SR, PT=200) | §6.4.1 | Y | Y | Generate + parse |
| 12 | Receiver Report (RR, PT=201) | §6.4.2 | Y | Y | Generate + parse |
| 13 | SR with report blocks | §6.4.1 | P | N | 28-byte SR only, no RB variant test |
| 14 | Extended highest sequence number | §6.4.1 | P | N | Not explicitly tested |
| 15 | Generic NACK (RFC 4585 §6.2.1) | RFC 4585 | Y | Y | `test_rtcp_nack_basic` |
| 16 | PLI (RFC 4585 §6.3.1) | RFC 4585 | Y | Y | `test_rtcp_pli_basic` |
| 17 | BYE packet (PT=203) | §6.6 | N | N | Not implemented |
| 18 | Compound RTCP packets | §6.1 | N | N | Not implemented |
| 19 | SRTCP protect | RFC 3711 | N | N | TODO in source |
| 20 | NACK bitmask interpretation | RFC 4585 §6.2.1 | Y | P | Basic test only, no multi-PID |

---

## RFC 3711 — SRTP

**File:** `src/nano_srtp.c` | **Tests:** `tests/test_srtp.c` (6 tests)

| # | Requirement | RFC | Impl | Test | Gap |
|---|-------------|-----|------|------|-----|
| 1 | AES-128-CM cipher | §4.1.1 | Y | P | No §B.1 key stream vector test |
| 2 | HMAC-SHA1-80 auth | §4.2.1 | Y | Y | Via protect/unprotect |
| 3 | Key derivation (6 labels) | §4.3.1 | Y | Y | RFC 3711 §B.3 vectors |
| 4 | ROC (Rollover Counter) | §3.3.1 | Y | N | No rollover boundary test |
| 5 | Replay detection | §3.3.2 | Y | Y | `test_srtp_tamper_detection` |
| 6 | SRTP protect/unprotect | §4 | Y | Y | Roundtrip test |
| 7 | SRTCP protect/unprotect | §4 | N | N | TODO in source |
| 8 | Key direction (client/server) | RFC 5764 | Y | Y | `test_srtp_key_direction` |

### RFC 3711 Appendix Test Vectors

| # | Vector | Test | Gap |
|---|--------|------|-----|
| 9 | §B.1 AES-CM key stream | N | Missing |
| 10 | §B.2 AES-CM encryption | N | Missing |
| 11 | §B.3 Key derivation | Y | `test_srtp_key_derivation_rfc3711_b3` |

---

## RFC 6184 — H.264 RTP Payload Format

**File:** `src/nano_h264.c` | **Tests:** `tests/test_h264.c` (22 tests)

| # | Requirement | RFC | Impl | Test | Gap |
|---|-------------|-----|------|------|-----|
| 1 | Single NAL Unit mode | §5.6 | Y | Y | `test_h264_pack_single_nal` |
| 2 | FU-A fragmentation (NAL > MTU) | §5.8 | Y | Y | Multiple FU-A tests |
| 3 | FU-A S/E bits | §5.8 | Y | Y | Start/End bit verification |
| 4 | FU-A NRI preservation | §5.8 | Y | Y | `test_h264_pack_fua_nri_preserved` |
| 5 | STAP-A depacketization | §5.7.1 | Y | Y | `test_h264_depkt_stapa` |
| 6 | STAP-A packetization (send) | §5.7.1 | N | N | Receive only |
| 7 | Keyframe detection (IDR, NAL type 5) | — | Y | Y | 9 keyframe tests |
| 8 | Forbidden bit (F=1) detection | §1.3 | P | N | Not explicitly tested |
| 9 | packetization-mode=1 | §6.2 | Y | Y | Via SDP fmtp |
| 10 | packetization-mode=0 | §6.1 | N | N | Not supported |

---

## RFC 7983 — Multiplexing

**File:** `src/nano_rtc.c` | **Tests:** `tests/test_e2e.c`

| # | Requirement | RFC | Impl | Test | Gap |
|---|-------------|-----|------|------|-----|
| 1 | First byte 0-3 → STUN | §3 | Y | Y | `test_e2e_demux_byte_ranges` |
| 2 | First byte 20-63 → DTLS | §3 | Y | Y | Same test |
| 3 | First byte 128-191 → RTP/RTCP | §3 | Y | Y | Same test |
| 4 | Byte range boundaries correct | §3 | Y | Y | All ranges tested |

---

## Summary

| RFC | Module | Impl Coverage | Test Coverage | Critical Gaps |
|-----|--------|--------------|---------------|---------------|
| RFC 8489 | STUN | 95% | 90% | Unknown attr rejection test |
| RFC 5769 | STUN Vectors | 100% | 100% | — |
| RFC 8445 | ICE | 75% (host, IPv4+IPv6) | 85% | Priority formula test, role conflict |
| RFC 6347 | DTLS | 80% | 70% | Content type, epoch, timeout tests |
| RFC 5764 | DTLS-SRTP | 90% | 80% | Keying material length test |
| RFC 4960 | SCTP | 75% | 70% | **Gap ack blocks**, RFC vectors, vtag test |
| RFC 3758 | PR-SCTP | 60% | 50% | FORWARD-TSN only |
| RFC 8832 | DCEP | 90% | 80% | — |
| RFC 8831 | DataChannel | 85% | 60% | E2E message exchange, PPID tests |
| RFC 8866 | SDP | 90% | 85% | Malformed rejection, BUNDLE, ice-lite |
| RFC 8829 | JSEP | 70% | 60% | Multi-mid BUNDLE test |
| RFC 3550 | RTP | 80% | 60% | Padding, extension, CSRC tests |
| RFC 3550 | RTCP | 70% | 70% | BYE, compound, SR+RB, extended seq |
| RFC 4585 | RTCP-FB | 80% | 75% | Multi-PID NACK bitmask |
| RFC 3711 | SRTP | 75% | 60% | §B.1/B.2 vectors, ROC rollover, SRTCP |
| RFC 6184 | H.264 | 75% | 85% | Forbidden bit, MTU boundary |
| RFC 7983 | Mux | 100% | 100% | — |
