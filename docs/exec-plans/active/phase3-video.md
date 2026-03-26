# Phase 3: Video Support

**Status:** Queued (blocked on Phase 2 completion)
**Estimated effort:** 2 agent sessions (~1-2 days elapsed)
**Goal:** ESP32 camera streaming to browser via H.264

## Acceptance Criteria

- [ ] H.264 FU-A packetization (fragmentation of NALUs over RTP)
- [ ] VP8 RTP packetization (optional)
- [ ] RTCP PLI (Picture Loss Indication) generation and handling
- [ ] RTCP REMB (optional bandwidth feedback)
- [ ] NAL unit reassembly in jitter buffer
- [ ] Keyframe detection and request mechanism
- [ ] SDP video m-line negotiation
- [ ] Basic bandwidth estimation
- [ ] Integration: H.264 stream from ESP32 to browser
- [ ] ESP32 example: camera push

## Implementation Steps

### Step 1: Video RTP + RTCP extensions + BWE (1 agent session)

| Task | File | RFC |
|------|------|-----|
| H.264 FU-A packetizer | `nano_rtp.c` | RFC 6184 §5.8 |
| H.264 NAL reassembly | `nano_jitter.c` | RFC 6184 §5.8 |
| VP8 packetizer (optional) | `nano_rtp.c` | RFC 7741 |
| RTCP PLI | `nano_rtcp.c` | RFC 4585 §6.3.1 |
| RTCP REMB | `nano_rtcp.c` | draft-alvestrand-rmcat-remb |
| SDP video m-line | `nano_sdp.c` | RFC 8866 |
| Basic bandwidth estimation | `nano_bwe.c` | — |

**Gate:** H.264 pack/unpack round-trip tests, PLI generation test

### Step 2: Integration + examples (1 agent session)

| Task | File |
|------|------|
| Keyframe request path | `nano_rtc.c` |
| Video path in main FSM | `nano_rtc.c` |
| E2E video test (synthetic, using sample H.264 frames) | `test_video.c` |
| Linux media send with H.264 samples | `examples/linux_media_send/` |
| ESP32 camera example | `examples/esp32_camera/` |

**Gate:** E2E video loopback in CI (sample frames)
**Human gate:** Browser H.264 playback verification, ESP32 camera test

## Risks

| Risk | Mitigation |
|------|-----------|
| H.264 FU-A edge cases | Use extensive pcap test vectors from real browser sessions |
| BWE complexity | Start with REMB-only; defer TWCC/GCC to Phase 4 |
| ESP32 camera frame rate | Target 10-15 fps initially; optimize later |
