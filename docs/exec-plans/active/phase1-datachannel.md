# Phase 1: DataChannel End-to-End

**Status:** Code Complete — interop tests pass (5/5), browser + ESP32 integration pending
**Estimated effort:** 4-6 agent sessions (~2-4 days elapsed)
**Goal:** NanoRTC ↔ browser DataChannel communication working on ESP32

## Effort Model

NanoRTC uses AI coding agents for implementation. Time estimates use **agent sessions** (one focused coding session, typically 2-6 hours of agent work) rather than human-weeks. The bottleneck is human review and browser-level verification, not coding speed.

| Unit | Meaning |
|------|---------|
| **Agent session** | One focused agent run: read RFC → implement → test → iterate until CI passes |
| **Human gate** | Requires human action: browser testing, design decision, hardware validation |

## Acceptance Criteria

### Unit tests (synthetic data, no network)
- [x] STUN Binding Request/Response with MESSAGE-INTEGRITY and FINGERPRINT
- [x] ICE connectivity: controlled role (answerer) and controlling role (offerer)
- [x] DTLS 1.2 handshake via mbedtls and OpenSSL crypto providers
- [x] SCTP four-way handshake (INIT → INIT-ACK → COOKIE-ECHO → COOKIE-ACK)
- [x] SCTP DATA/SACK reliable delivery
- [x] DCEP DATA_CHANNEL_OPEN/ACK exchange
- [x] DataChannel string and binary messages flow bidirectionally
- [x] SDP offer/answer with DataChannel m-line
- [x] All unit tests pass with synthetic data (no network)

### Interop tests (libdatachannel over localhost UDP) — **mandatory gate**
- [x] `test_interop_handshake` — Full ICE + DTLS + SCTP handshake with libdatachannel
- [x] `test_interop_dc_open` — DataChannel opens on both sides
- [x] `test_interop_dc_string_libdatachannel_to_nanortc` — Text message from libdatachannel → nanortc
- [x] `test_interop_dc_string_nanortc_to_libdatachannel` — Text message from nanortc → libdatachannel
- [x] `test_interop_dc_binary` — Binary data from libdatachannel → nanortc

### Integration tests (human gate)
- [ ] Integration test: NanoRTC ↔ browser on Linux (`examples/linux_datachannel`)
- [ ] ESP32 example: HTTP signaling + DataChannel echo (`examples/esp32_datachannel/`)

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

**Gate:** SCTP association in e2e loopback test ✓

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

**Gate:** E2E DataChannel loopback in CI ✓
**Human gate:** Browser DataChannel echo test, ESP32 hardware test

### Step 5: Interop testing against libdatachannel (1 agent session)

| Task | File | Tests |
|------|------|-------|
| Interop test framework setup | `tests/interop/CMakeLists.txt` | FetchContent for libdatachannel v0.22.5 |
| Signaling pipe (socketpair) | `interop_common.{h,c}` | SDP/ICE exchange over AF_UNIX |
| nanortc peer wrapper (thread + run_loop) | `interop_nanortc_peer.{h,c}` | Answerer role with real UDP socket |
| libdatachannel peer wrapper (C API) | `interop_libdatachannel_peer.{h,c}` | Offerer role with internal threads |
| DataChannel interop tests | `test_interop_dc.c` | Handshake, DC open, text/binary messages |

**Gate:** All 5 interop tests pass (`ctest -R interop`)
**Dependency:** Steps 3-4 must be complete (SCTP + DataChannel + SDP fully working)

## Decision Log

| Date | Decision | Rationale |
|------|----------|-----------|
| 2026-03-26 | Dual crypto backend (mbedtls + OpenSSL) | mbedtls for embedded, OpenSSL for Linux host dev/CI |
| 2026-03-26 | ICE supports both controlled and controlling roles | Device can be offerer or answerer |
| 2026-03-26 | Agent session-based planning | Coding speed is not the bottleneck; human verification is |
| 2026-03-26 | CRC-32 vs CRC-32c: separate modules | STUN FINGERPRINT uses ISO HDLC (0xEDB88320); SCTP uses Castagnoli (0x82F63B78) |
| 2026-03-26 | RFC 5769 test vectors mandatory | Byte-level interop verification — roundtrip tests alone cannot catch shared encoder/parser bugs |
| 2026-03-26 | MI/FP ordering enforced in parser | After MI: ignore all attrs except FP. After FP: reject any further attrs (RFC 8489 §14.5/§14.7) |
| 2026-03-27 | libdatachannel as interop reference peer | Known-good C/C++ WebRTC implementation with C API; validates full protocol stack over real UDP. apt not available; fetched via CMake FetchContent. |
| 2026-03-27 | Interop tests as mandatory Phase 1 gate | Unit tests alone cannot catch SDP format mismatches, DTLS parameter negotiation bugs, or SCTP interop issues. Interop tests are required for Phase 1 sign-off. |
| 2026-03-27 | Renamed `sctp_` to `nsctp_` prefix | Avoid usrsctp symbol collision in interop builds where both libraries are linked |
| 2026-03-27 | Named array size macros mandatory | All struct arrays use `NANO_*` or `MODULE_*_SIZE` macros — enforced by CI |
| 2026-03-27 | ESP32 signaling: HTTP instead of MQTT | Zero new code (`http_signaling.c` already exists with ESP32 compat), zero external deps (no MQTT broker), unified test flow with `signaling_server.py` |

## Progress

### Step 1: STUN + ICE (Completed 2026-03-26, 1 session)

**Implemented:**
- STUN message codec (RFC 8489): full parser/encoder with all ICE attributes
- MESSAGE-INTEGRITY (HMAC-SHA1) via crypto provider interface
- FINGERPRINT (CRC-32 ISO HDLC, separate from CRC-32c for SCTP)
- ICE controlled role: validate incoming Binding Requests, respond with XOR-MAPPED-ADDRESS
- ICE controlling role: generate Binding Requests with pacing (50ms), process responses
- USE-CANDIDATE nomination (RFC 8445 §7.2.1.4)
- Main FSM: RFC 7983 packet demux + timeout-driven ICE checks
- Crypto backends: hmac_sha1 + random_bytes implemented for both mbedtls and OpenSSL

**Tests (69 total across 4 suites):**
- RFC 5769 byte-level test vectors (§2.1 request, §2.2 IPv4 response, §2.3 IPv6 response)
- str0m real-world Binding Request (with unknown NETWORK-COST attribute)
- ICE roundtrip: controlling → controlled → response → both CONNECTED
- MI/FP ordering enforcement (RFC 8489 §14.5/§14.7)
- Edge cases: bad credentials, pacing, max checks → FAILED, ERROR-CODE parsing

**Files created/modified:** nano_crc32.c/h (new), nano_stun.c/h, nano_ice.c/h, nano_rtc.c, nano_rtc_internal.h, nano_crypto_openssl.c, nano_crypto_mbedtls.c, CMakeLists.txt, 4 test files

### Step 2: DTLS integration (Completed 2026-03-26, 1 session)

**Implemented:**
- Sans I/O BIO adapter (`nano_dtls.c`): bio_send_cb/bio_recv_cb bridge crypto providers with NanoRTC buffers
- ECDSA P-256 self-signed certificate generation (both backends)
- SHA-256 certificate fingerprint computation ("XX:XX:..." format for SDP)
- DTLS 1.2 handshake via mbedtls 3.5 and OpenSSL 3.0
- Application data encrypt/decrypt (post-handshake DTLS records)
- SRTP keying material export (RFC 5764 "EXTRACTOR-dtls_srtp" label, 60 bytes)
- FSM integration: ICE_CONNECTED → dtls_init + dtls_start → DTLS_HANDSHAKING → DTLS_CONNECTED
- DTLS demux in nano_handle_receive (RFC 7983 byte range 0x14-0x3F)

**Tests (79 total across 5 suites, 10 new):**
- DTLS init/destroy for server and client roles
- SHA-256 fingerprint format validation (95 chars, hex:colon format)
- Fingerprint uniqueness (two instances generate different certs)
- Two-instance handshake loopback (client ↔ server in memory, ~4-6 rounds)
- Encrypt on client → decrypt on server, and reverse
- Both sides derive identical 60-byte keying material
- State validation (no encrypt before handshake, no double start)
- E2E ICE → DTLS loopback via nano_rtc FSM

**Key decisions:**
- ECDSA P-256 over RSA: smaller certs (fits 2KB buffer), faster keygen on embedded
- Cookies disabled: WebRTC uses ICE for peer verification, DTLS cookies add complexity without security benefit
- Heap allocation in crypto providers: mbedtls/OpenSSL need internal malloc; `crypto/` is allowed
- mbedtls 3.x API: `mbedtls_ssl_set_export_keys_cb` replaces 2.x `_ext_cb`

**Files created/modified:** nano_dtls.c/h, nano_crypto.h, nano_crypto_mbedtls.c, nano_crypto_openssl.c, nano_rtc.c, nano_rtc_internal.h, test_dtls.c (new), test_e2e.c

### Step 3: SCTP-Lite (Completed 2026-03-27, 1 session)

**Implemented:**
- Full SCTP chunk codec (RFC 4960 §3): INIT, INIT-ACK, COOKIE-ECHO, COOKIE-ACK, DATA, SACK, HEARTBEAT, HEARTBEAT-ACK, FORWARD-TSN, SHUTDOWN, ABORT
- Four-way handshake FSM (INIT → INIT-ACK → COOKIE-ECHO → COOKIE-ACK)
- DATA/SACK reliable delivery with ordered stream support
- Retransmission timer (T3-rtx) with exponential backoff
- FORWARD-TSN for unreliable DataChannels (RFC 3758)
- HEARTBEAT keepalive with timer-driven exchange
- Ring output queue for outbound chunk buffering
- CRC-32c (Castagnoli) for SCTP checksums, separate from CRC-32 (STUN)

**Tests (27 across codec, CRC, handshake, data exchange, SACK, FORWARD-TSN, output queue):**
- Chunk encode/decode roundtrip for all chunk types
- CRC-32c test vectors (Castagnoli polynomial)
- Full four-way handshake between two `nsctp_assoc_t` instances
- DATA send → SACK receive → TSN advance
- Retransmit on T3-rtx timeout
- FORWARD-TSN skip and receiver TSN update
- Output queue ring buffer fill/drain

**Key files:** `nano_sctp.c/h` (renamed to `nsctp_` prefix), `nano_crc32c.c/h`
**Missing (non-critical for Phase 1):** Gap tracking, RECONFIG, SHUTDOWN-ACK

### Step 4: DataChannel + SDP + Integration (Completed 2026-03-27, 1 session)

**Implemented:**
- DCEP codec (RFC 8832): DATA_CHANNEL_OPEN/ACK parse and encode
- DataChannel FSM: CLOSED → OPENING → OPEN → CLOSING
- Bidirectional string/binary message routing via SCTP PPID dispatch
- SDP parser: handles Chrome, Firefox, Safari offer formats
- SDP generator: creates valid WebRTC answer with DataChannel m-line
- `nano_accept_offer` integration in main FSM (SDP parse → ICE/DTLS/SCTP config)
- E2E loopback test: two `nano_rtc_t` instances exchange DataChannel messages

**Tests (11 SDP tests + DCEP/e2e):**
- Chrome/Firefox/Safari SDP offer parsing
- SDP generator output validation
- SDP roundtrip (generate → parse own output)
- `nano_accept_offer` full pipeline test
- E2E DataChannel string/binary loopback (CI-passing)

**Key files:** `nano_datachannel.c/h`, `nano_sdp.c/h`, `nano_rtc.c`, `test_sdp.c`, `test_e2e.c`
**Note:** E2E DataChannel loopback passes in CI

### Step 5: Interop test framework (Completed 2026-03-27, 1 session)

**Implemented:**
- Interop test framework using libdatachannel v0.22.5 as reference WebRTC peer
- Single-process, dual-threaded architecture: nanortc (answerer) + libdatachannel (offerer)
- Signaling via socketpair: SDP offer/answer + ICE candidates with framed message protocol
- nanortc peer wrapper: reuses `run_loop_linux.c` event loop in a dedicated thread
- libdatachannel peer wrapper: C API callbacks with thread-safe state observation
- 5 test cases: handshake, DC open, bidirectional text/binary messages
- CMake integration via FetchContent (OFF by default: `NANORTC_BUILD_INTEROP_TESTS`)
- Resolves sctp_init symbol collision with usrsctp via `--allow-multiple-definition`

**Tests (5 interop test cases):**
- `test_interop_handshake` — Full ICE → DTLS → SCTP handshake
- `test_interop_dc_open` — DataChannel opens on both sides
- `test_interop_dc_string_libdatachannel_to_nanortc` — Text message libdatachannel → nanortc
- `test_interop_dc_string_nanortc_to_libdatachannel` — Text message nanortc → libdatachannel
- `test_interop_dc_binary` — Binary payload (256 bytes) libdatachannel → nanortc

**Status:** All 5 interop tests pass (`ctest -R interop`, ~6s). SDP compatibility fixed in commit `4b5f7bb`. Remaining: browser integration (human gate) and ESP32 hardware test.

**Files created:** tests/interop/ (8 new files), CMakeLists.txt modified

## Risks

| Risk | Mitigation |
|------|-----------|
| SCTP complexity exceeds 1 session | Split into parser session + FSM session; reference str0m |
| mbedtls DTLS BIO integration issues | Reference mbedtls examples for BIO pattern |
| OpenSSL DTLS BIO adapter differs from mbedtls | Abstract BIO in `nano_dtls.c`, test both backends in CI |
| Browser SDP format variations | Test with Chrome, Firefox, Safari; parse conservatively |
| libdatachannel SDP differs from browser SDP | Interop tests catch this; fix SDP parser to handle both |
| sctp_init symbol collision with usrsctp | Resolved via `--allow-multiple-definition` linker flag in interop CMake |
