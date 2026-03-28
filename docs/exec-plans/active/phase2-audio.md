# Phase 2: Audio Support

**Status:** Active — Session 3 complete (browser audio verified)
**Estimated effort:** 2-3 agent sessions (~1-2 days elapsed)
**Goal:** Bidirectional audio between NanoRTC (ESP32) and browser

## Acceptance Criteria

- [x] SRTP key derivation from DTLS keying material
- [x] SRTP encrypt/decrypt with AES-128-CM + HMAC-SHA1-80
- [x] RTP packing for Opus and G.711 (PCMA/PCMU)
- [x] RTP unpacking with payload type mapping
- [x] RTCP Sender Report (SR) generation
- [x] RTCP Receiver Report (RR) generation
- [x] RTCP NACK generation and handling
- [x] Jitter buffer with configurable depth
- [x] SDP audio m-line negotiation
- [x] Integration: nanortc → Chrome audio verified (Opus, 0% concealed samples)
- [ ] Integration: Chrome → nanortc audio receive (bidirectional)
- [ ] ESP32 example: audio intercom

## Implementation Steps

### Step 1: SRTP + RTP (1 agent session)

| Task | File | RFC |
|------|------|-----|
| SRTP key derivation | `nano_srtp.c` | RFC 3711 §4.3 |
| AES-128-CM via crypto provider | `nano_srtp.c` | RFC 3711 §4.1 |
| SRTP protect/unprotect | `nano_srtp.c` | RFC 3711 §3 |
| Replay protection (sliding window) | `nano_srtp.c` | RFC 3711 §3.3 |
| RTP header encode/decode | `nano_rtp.c` | RFC 3550 §5 |
| Opus packetization | `nano_rtp.c` | RFC 7587 |
| G.711 packetization | `nano_rtp.c` | RFC 3551 §4.5 |

**Gate:** SRTP round-trip test, RTP pack/unpack tests in CI — **PASSED**
**Post-gate fixes (Session 3):** SRTP IV computation (bytes 6-13, RFC 3711 §4.1.1), RTP marker=1 for first frame, random initial seq (RFC 3550), SDP ptime:20

### Step 2: RTCP + Jitter Buffer + SDP Audio (1 agent session) — **COMPLETE**

| Task | File | RFC | Status |
|------|------|-----|--------|
| RTCP SR generation | `nano_rtcp.c` | RFC 3550 §6.4.1 | Done — 15 tests |
| RTCP RR generation | `nano_rtcp.c` | RFC 3550 §6.4.2 | Done |
| RTCP NACK | `nano_rtcp.c` | RFC 4585 §6.2.1 | Done |
| Jitter buffer (fixed ring) | `nano_jitter.c` | — | Done — 8 tests |
| Sequence reordering | `nano_jitter.c` | — | Done |
| SDP audio m-line | `nano_sdp.c` | RFC 8866 | Done |
| Audio path in main FSM | `nano_rtc.c` | — | Done — media demux + RTCP timer |
| Consistency fixes | multiple | — | Done — byte-order helpers, config macros, types, logging |

**Gate:** All 6 feature combos build + test — **PASSED**
**Remaining:** Chrome → nanortc receive path, ESP32 audio intercom (Step 3)

### Step 3: ESP32 example (1 agent session, optional — can parallel)

| Task | File |
|------|------|
| Linux media send with Opus samples | `examples/linux_media_send/` |
| ESP32 audio intercom | `examples/esp32_audio_intercom/` |

**Human gate:** ESP32 hardware audio test

## Risks

| Risk | Mitigation |
|------|-----------|
| SRTP crypto provider gaps | Test mbedtls + OpenSSL AES-CM and HMAC paths in Step 1 |
| Jitter buffer tuning | Start with fixed 100ms depth; parameterize for later optimization |
