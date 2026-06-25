# ESP32 Media Example

ESP32 WebRTC A/V sender demo using nanortc. The ESP32 streams **pre-encoded**
H.264 video and/or Opus audio from flash to a browser over WebRTC. The media
blobs are auto-packed from `examples/sample_data/` at build time (no SD card),
and the device hosts its own signaling page — no external signaling server.

Which tracks are sent is selected at build time:

| menuconfig option        | Default | Effect                                            |
|--------------------------|---------|---------------------------------------------------|
| `EXAMPLE_ENABLE_VIDEO`   | `y`     | Pack the H.264 blob and send a video track        |
| `EXAMPLE_ENABLE_AUDIO`   | `y`     | Pack the Opus blob and send audio                 |
| `EXAMPLE_VIDEO_FPS`      | `25`    | Video playback rate (only when video enabled)     |
| `EXAMPLE_UDP_PORT`       | `9999`  | Local UDP port for STUN/DTLS/RTP                   |

This example plays back **pre-encoded** media from flash (no live encoding
on-device).

## Track selection

nanortc (the answerer) matches local tracks to the offer's m-lines **by
position**. To keep that alignment in every build, the browser page first
calls **`GET /config`** — which reports the enabled tracks — and offers only
those, in the same order the firmware registers them (audio first, then
video). So `both` / `video-only` / `audio-only` all negotiate cleanly with no
placeholder m-line. At least one track must be enabled (enforced at compile
time).

## Hardware

Any ESP32 / ESP32-S3 with WiFi and enough flash for the embedded media blobs
(see `partitions.csv`). No camera, codec, or SD card required.

## Build & Run

```bash
cd examples/esp32_media
idf.py menuconfig    # Example Connection Configuration → WiFi SSID/password
                     # ESP32 Media Example → enable/disable video / audio
idf.py build flash monitor
```

Then open `http://<esp32-ip>/` (IP printed on the serial console) and click
**Connect** to start the WebRTC session.

## Architecture

```
Browser                          ESP32
  │  GET /  ──────────────────→    │  Serve index.html (embedded)
  │  POST /offer (SDP) ──────→     │  nano_session_accept_offer()
  │  ←──── SDP answer ─────────    │
  │  ═══ WebRTC (STUN/DTLS/RTP) ══ │  Direct UDP on LAN
  │  ←──── H.264 / Opus ───────    │  paced from flash blobs, sendonly
                                   │  video.blob / audio.blob (pack_frames.py)
```

## Requirements

- **ESP-IDF v5.4+** with `CONFIG_MBEDTLS_SSL_DTLS_SRTP=y` (set in `sdkconfig.defaults`).
- Python (build host) for `tools/pack_frames.py`, run automatically by CMake.
