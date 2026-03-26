# Phase 2: Audio Support

**Status:** Queued (blocked on Phase 1 completion)
**Estimated duration:** 2-3 weeks
**Goal:** Bidirectional audio between NanoRTC (ESP32) and browser

## Acceptance Criteria

- [ ] SRTP key derivation from DTLS keying material
- [ ] SRTP encrypt/decrypt with AES-128-CM + HMAC-SHA1-80
- [ ] RTP packing for Opus and G.711 (PCMA/PCMU)
- [ ] RTP unpacking with payload type mapping
- [ ] RTCP Sender Report (SR) generation
- [ ] RTCP Receiver Report (RR) generation
- [ ] RTCP NACK generation and handling
- [ ] Jitter buffer with configurable depth
- [ ] SDP audio m-line negotiation
- [ ] Integration: bidirectional audio with browser
- [ ] ESP32 example: audio intercom

## Module Implementation Order

### Week 1: SRTP + RTP

| Task | File | RFC |
|------|------|-----|
| SRTP key derivation | `nano_srtp.c` | RFC 3711 §4.3 |
| AES-128-CM via crypto provider | `nano_srtp.c` | RFC 3711 §4.1 |
| SRTP protect/unprotect | `nano_srtp.c` | RFC 3711 §3 |
| Replay protection (sliding window) | `nano_srtp.c` | RFC 3711 §3.3 |
| RTP header encode/decode | `nano_rtp.c` | RFC 3550 §5 |
| Opus packetization | `nano_rtp.c` | RFC 7587 |
| G.711 packetization | `nano_rtp.c` | RFC 3551 §4.5 |

### Week 2: RTCP + Jitter Buffer

| Task | File | RFC |
|------|------|-----|
| RTCP SR generation | `nano_rtcp.c` | RFC 3550 §6.4.1 |
| RTCP RR generation | `nano_rtcp.c` | RFC 3550 §6.4.2 |
| RTCP NACK | `nano_rtcp.c` | RFC 4585 §6.2.1 |
| Jitter buffer (fixed ring) | `nano_jitter.c` | — |
| Sequence reordering | `nano_jitter.c` | — |
| SDP audio m-line | `nano_sdp.c` | RFC 8866 |

### Week 3: Integration + ESP32

| Task | File |
|------|------|
| Audio path in main FSM | `nano_rtc.c` |
| E2E audio test (synthetic) | `test_audio.c` |
| Linux audio example | `examples/linux_echo/` |
| ESP32 audio intercom | `examples/esp32_audio_intercom/` |

## Risks

| Risk | Mitigation |
|------|-----------|
| SRTP crypto provider gaps | Test mbedtls AES-CM and HMAC paths early in week 1 |
| Jitter buffer tuning | Start with fixed 100ms depth; parameterize for later optimization |
