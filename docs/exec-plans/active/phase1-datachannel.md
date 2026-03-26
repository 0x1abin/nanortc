# Phase 1: DataChannel End-to-End

**Status:** Active
**Estimated effort:** 4-6 agent sessions (~2-4 days elapsed)
**Goal:** NanoRTC ↔ browser DataChannel communication working on ESP32

## Effort Model

NanoRTC uses AI coding agents for implementation. Time estimates use **agent sessions** (one focused coding session, typically 2-6 hours of agent work) rather than human-weeks. The bottleneck is human review and browser-level verification, not coding speed.

| Unit | Meaning |
|------|---------|
| **Agent session** | One focused agent run: read RFC → implement → test → iterate until CI passes |
| **Human gate** | Requires human action: browser testing, design decision, hardware validation |

## Acceptance Criteria

- [ ] STUN Binding Request/Response with MESSAGE-INTEGRITY and FINGERPRINT
- [ ] ICE connectivity: controlled role (answerer) and controlling role (offerer)
- [ ] DTLS 1.2 handshake via mbedtls and OpenSSL crypto providers
- [ ] SCTP four-way handshake (INIT → INIT-ACK → COOKIE-ECHO → COOKIE-ACK)
- [ ] SCTP DATA/SACK reliable delivery
- [ ] DCEP DATA_CHANNEL_OPEN/ACK exchange
- [ ] DataChannel string and binary messages flow bidirectionally
- [ ] SDP offer/answer with DataChannel m-line
- [ ] All unit tests pass with synthetic data (no network)
- [ ] Integration test: NanoRTC ↔ browser on Linux (`examples/linux_datachannel`)
- [ ] ESP32 example: MQTT signaling + DataChannel echo

## Implementation Steps

### Step 1: STUN + ICE (1 agent session)

| Task | File | RFC | Tests |
|------|------|-----|-------|
| STUN message parser | `nano_stun.c` | RFC 8489 §5-6 | Decode known packets from pcap |
| STUN message encoder | `nano_stun.c` | RFC 8489 §5-6 | Round-trip encode→decode |
| MESSAGE-INTEGRITY (HMAC-SHA1) | `nano_stun.c` | RFC 8489 §14 | Verify against RFC test vectors |
| FINGERPRINT (CRC-32 XOR) | `nano_stun.c` | RFC 8489 §14 | Known CRC values |
| ICE controlled role (answerer) | `nano_ice.c` | RFC 8445 §2.2 | Respond to STUN Binding Request |
| ICE controlling role (offerer) | `nano_ice.c` | RFC 8445 §7 | Initiate STUN Binding Request, process response |

**Gate:** CI passes, STUN round-trip tests green

### Step 2: DTLS integration (1 agent session)

| Task | File | RFC | Tests |
|------|------|-----|-------|
| mbedtls DTLS init (server mode) | `nano_crypto_mbedtls.c` | RFC 6347 | Handshake with openssl s_client |
| OpenSSL DTLS init (server mode) | `nano_crypto_openssl.c` | RFC 6347 | Handshake with browser |
| BIO adapter (Sans I/O ↔ crypto provider) | `nano_dtls.c` | — | Feed handshake bytes, verify output |
| Self-signed certificate generation | `nano_crypto_*.c` | — | Fingerprint matches SDP |
| Keying material export | `nano_crypto_*.c` | RFC 5764 | Verify SRTP key derivation |

**Gate:** DTLS handshake two-instance loopback test passes (both backends)
**Human gate:** Browser DTLS handshake verification

### Step 3: SCTP-Lite (1-2 agent sessions)

SCTP is the most complex module (~2500 lines). May need multiple sessions.

| Task | File | RFC | Tests |
|------|------|-----|-------|
| SCTP chunk parser/encoder | `nano_sctp.c` | RFC 4960 §3 | Round-trip all chunk types |
| Four-way handshake FSM | `nano_sctp.c` | RFC 4960 §5 | Two nano_sctp_t instances back-to-back |
| DATA/SACK processing | `nano_sctp.c` | RFC 4960 §6 | Ordered delivery, gap detection |
| Retransmission timer | `nano_sctp.c` | RFC 4960 §6.3 | Simulate packet loss |
| FORWARD-TSN (unreliable DC) | `nano_sctp.c` | RFC 3758 | Unordered message delivery |
| HEARTBEAT keepalive | `nano_sctp.c` | RFC 4960 §8.3 | Timer-driven heartbeat exchange |

**Gate:** SCTP association in e2e loopback test

### Step 4: DataChannel + SDP + Integration (1 agent session)

| Task | File | RFC | Tests |
|------|------|-----|-------|
| DCEP parser/encoder | `nano_datachannel.c` | RFC 8832 | DATA_CHANNEL_OPEN round-trip |
| Channel open/ack FSM | `nano_datachannel.c` | RFC 8832 | Open → ACK → OPEN state |
| String/binary message routing | `nano_datachannel.c` | RFC 8831 | PPID-based type dispatch |
| SDP parser (offer) | `nano_sdp.c` | RFC 8866 | Parse Chrome-generated SDP |
| SDP generator (answer) | `nano_sdp.c` | RFC 8866 | Generate valid answer |
| Main FSM integration | `nano_rtc.c` | — | Full packet flow test |
| E2E test (two instances) | `test_e2e.c` | — | Synthetic DataChannel exchange |
| Linux echo integration | `examples/linux_datachannel/` | — | Real UDP + stdin signaling |

**Gate:** E2E DataChannel loopback in CI
**Human gate:** Browser DataChannel echo test, ESP32 hardware test

## Decision Log

| Date | Decision | Rationale |
|------|----------|-----------|
| 2026-03-26 | Dual crypto backend (mbedtls + OpenSSL) | mbedtls for embedded, OpenSSL for Linux host dev/CI |
| 2026-03-26 | ICE supports both controlled and controlling roles | Device can be offerer or answerer |
| 2026-03-26 | Agent session-based planning | Coding speed is not the bottleneck; human verification is |

## Progress

_Updated as implementation proceeds._

## Risks

| Risk | Mitigation |
|------|-----------|
| SCTP complexity exceeds 1 session | Split into parser session + FSM session; reference libpeer and str0m |
| mbedtls DTLS BIO integration issues | Reference libpeer's `dtls_srtp.c` for proven BIO pattern |
| OpenSSL DTLS BIO adapter differs from mbedtls | Abstract BIO in `nano_dtls.c`, test both backends in CI |
| Browser SDP format variations | Test with Chrome, Firefox, Safari; parse conservatively |
