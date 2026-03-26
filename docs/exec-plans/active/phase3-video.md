# Phase 3: Video Support

**Status:** Queued (blocked on Phase 2 completion)
**Estimated duration:** 2-3 weeks
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

## Module Implementation Order

### Week 1: Video RTP + RTCP Extensions

| Task | File | RFC |
|------|------|-----|
| H.264 FU-A packetizer | `nano_rtp.c` | RFC 6184 §5.8 |
| H.264 NAL reassembly | `nano_jitter.c` | RFC 6184 §5.8 |
| VP8 packetizer (optional) | `nano_rtp.c` | RFC 7741 |
| RTCP PLI | `nano_rtcp.c` | RFC 4585 §6.3.1 |
| RTCP REMB | `nano_rtcp.c` | draft-alvestrand-rmcat-remb |
| SDP video m-line | `nano_sdp.c` | RFC 8866 |

### Week 2: BWE + Integration

| Task | File |
|------|------|
| Basic bandwidth estimation | `nano_bwe.c` |
| Keyframe request path | `nano_rtc.c` |
| Video path in main FSM | `nano_rtc.c` |
| E2E video test (synthetic) | `test_video.c` |

### Week 3: ESP32 Example

| Task | File |
|------|------|
| Linux video example | `examples/linux_echo/` |
| ESP32 camera example | `examples/esp32_camera/` |
| ESP32 camera + audio combined | `examples/esp32_camera/` |

## Risks

| Risk | Mitigation |
|------|-----------|
| H.264 FU-A edge cases | Use extensive pcap test vectors from real browser sessions |
| BWE complexity | Start with REMB-only; defer TWCC/GCC to Phase 4 |
| ESP32 camera frame rate | Target 10-15 fps initially; optimize later |
