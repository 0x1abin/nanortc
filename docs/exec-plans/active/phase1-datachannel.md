# Phase 1: DataChannel End-to-End

**Status:** Active
**Estimated duration:** 3-4 weeks
**Goal:** NanoRTC ↔ browser DataChannel communication working on ESP32

## Acceptance Criteria

- [ ] STUN Binding Request/Response with MESSAGE-INTEGRITY and FINGERPRINT
- [ ] ICE connectivity: controlled role (answerer, ICE-Lite) and controlling role (offerer)
- [ ] DTLS 1.2 handshake completes via mbedtls crypto provider
- [ ] SCTP four-way handshake (INIT → INIT-ACK → COOKIE-ECHO → COOKIE-ACK)
- [ ] SCTP DATA/SACK reliable delivery
- [ ] DCEP DATA_CHANNEL_OPEN/ACK exchange
- [ ] DataChannel string and binary messages flow bidirectionally
- [ ] SDP offer/answer with DataChannel m-line
- [ ] All unit tests pass with synthetic data (no network)
- [ ] Integration test: NanoRTC ↔ browser on Linux
- [ ] ESP32 example: MQTT signaling + DataChannel echo

## Module Implementation Order

### Week 1: STUN + ICE

| Task | File | RFC | Tests |
|------|------|-----|-------|
| STUN message parser | `nano_stun.c` | RFC 8489 §5-6 | Decode known packets from pcap |
| STUN message encoder | `nano_stun.c` | RFC 8489 §5-6 | Round-trip encode→decode |
| MESSAGE-INTEGRITY (HMAC-SHA1) | `nano_stun.c` | RFC 8489 §14 | Verify against RFC test vectors |
| FINGERPRINT (CRC-32 XOR) | `nano_stun.c` | RFC 8489 §14 | Known CRC values |
| ICE controlled role (answerer) | `nano_ice.c` | RFC 8445 §2.2 | Respond to STUN Binding Request |
| ICE controlling role (offerer) | `nano_ice.c` | RFC 8445 §7 | Initiate STUN Binding Request, process response |

**Milestone:** ICE connectivity works in both roles with Chrome/Firefox

### Week 2: DTLS Integration

| Task | File | RFC | Tests |
|------|------|-----|-------|
| mbedtls DTLS init (server mode) | `nano_crypto_mbedtls.c` | RFC 6347 | Handshake with openssl s_client |
| BIO adapter (Sans I/O ↔ mbedtls) | `nano_dtls.c` | — | Feed handshake bytes, verify output |
| Self-signed certificate generation | `nano_crypto_mbedtls.c` | — | Fingerprint matches SDP |
| Keying material export | `nano_crypto_mbedtls.c` | RFC 5764 | Verify SRTP key derivation |

**Milestone:** DTLS handshake completes with browser

### Week 3: SCTP-Lite

| Task | File | RFC | Tests |
|------|------|-----|-------|
| SCTP chunk parser/encoder | `nano_sctp.c` | RFC 4960 §3 | Round-trip all chunk types |
| Four-way handshake FSM | `nano_sctp.c` | RFC 4960 §5 | Two nano_sctp_t instances back-to-back |
| DATA/SACK processing | `nano_sctp.c` | RFC 4960 §6 | Ordered delivery, gap detection |
| Retransmission timer | `nano_sctp.c` | RFC 4960 §6.3 | Simulate packet loss |
| FORWARD-TSN (unreliable DC) | `nano_sctp.c` | RFC 3758 | Unordered message delivery |
| HEARTBEAT keepalive | `nano_sctp.c` | RFC 4960 §8.3 | Timer-driven heartbeat exchange |

**Milestone:** SCTP association establishes over DTLS

### Week 4: DataChannel + SDP + Integration

| Task | File | RFC | Tests |
|------|------|-----|-------|
| DCEP parser/encoder | `nano_datachannel.c` | RFC 8832 | DATA_CHANNEL_OPEN round-trip |
| Channel open/ack FSM | `nano_datachannel.c` | RFC 8832 | Open → ACK → OPEN state |
| String/binary message routing | `nano_datachannel.c` | RFC 8831 | PPID-based type dispatch |
| SDP parser (offer) | `nano_sdp.c` | RFC 8866 | Parse Chrome-generated SDP |
| SDP generator (answer) | `nano_sdp.c` | RFC 8866 | Generate valid answer |
| Main FSM integration | `nano_rtc.c` | — | Full packet flow test |
| E2E test (two instances) | `test_e2e.c` | — | Synthetic DataChannel exchange |
| Linux echo example | `examples/linux_echo/` | — | Real UDP + signaling |

**Milestone:** DataChannel echo working with browser

## Decision Log

| Date | Decision | Rationale |
|------|----------|-----------|

## Progress

_Updated as implementation proceeds._

## Risks

| Risk | Mitigation |
|------|-----------|
| SCTP complexity exceeds estimate | Start with minimum viable subset; defer fragmentation and multi-stream to follow-up |
| mbedtls DTLS BIO integration issues | Reference libpeer's `dtls_srtp.c` for proven BIO pattern |
| Browser SDP format variations | Test with Chrome, Firefox, Safari; parse conservatively |
