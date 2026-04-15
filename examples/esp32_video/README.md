# ESP32 Video Example

ESP32 WebRTC video sender demo using nanortc. The ESP32 reads pre-encoded
H.264 frames from an SD card and streams them to the browser via WebRTC.
Zero external dependencies — no signaling server needed.

## Hardware

- **Board:** ESP32-Korvo-2 V3 (ESP32-S3, 8MB PSRAM)
- **SD card:** FAT32 formatted, containing H.264 sample frames

## Quick Start

### 1. Prepare SD Card

Copy `examples/sample_data/h264SampleFrames/` to your SD card:
```
SD card root/
└── h264SampleFrames/
    ├── frame-0001.h264
    ├── frame-0002.h264
    └── ... (1500 frames, Annex-B format, 25fps)
```

### 2. Configure

```bash
cd examples/esp32_video
idf.py menuconfig
# → "Example Connection Configuration" → set WiFi SSID and Password
# → "ESP32 Video Example" → adjust SD card pins if needed
```

### 3. Build and Flash

```bash
idf.py build
idf.py flash monitor
```

### 4. Open in Browser

Navigate to `http://<esp32-ip>/` (the IP is printed on the serial console).
Click **Connect** to start the WebRTC session and watch the video.

## SD Card Pin Configuration

Default pins for ESP32-Korvo-2 V3 (1-line SDMMC mode). Adjust via menuconfig
if your board uses different pins:

| Signal | Default GPIO | Kconfig |
|--------|-------------|---------|
| CMD | 7 | `EXAMPLE_SD_CMD_PIN` |
| CLK | 15 | `EXAMPLE_SD_CLK_PIN` |
| D0 | 4 | `EXAMPLE_SD_D0_PIN` |

## Architecture

```
Browser                          ESP32
  │                                │
  │  GET /  ──────────────────→    │  Serve index.html
  │  ←──────────────────────────   │
  │                                │
  │  POST /offer (SDP) ──────→     │  nanortc_accept_offer()
  │  ←──── SDP answer ─────────    │
  │                                │
  │  ═══ WebRTC (STUN/DTLS/RTP) ══ │  Direct UDP on LAN
  │                                │
  │  ←──── Video (H.264) ───────   │  25fps, sendonly
  │                                │
  │         SD Card ──→ read frame │  frame-NNNN.h264
  │                    NAL split   │  nano_annex_b_find_nal()
  │                    RTP pack    │  nanortc_send_video()
```

## Configuration

| Setting | Default | Description |
|---------|---------|-------------|
| `EXAMPLE_UDP_PORT` | `9999` | Local UDP port for WebRTC |
| `EXAMPLE_VIDEO_FPS` | `25` | Playback frame rate |
| `EXAMPLE_H264_DIR` | `h264SampleFrames` | Directory name on SD card |
| `EXAMPLE_SD_CMD_PIN` | `7` | SDMMC CMD GPIO |
| `EXAMPLE_SD_CLK_PIN` | `15` | SDMMC CLK GPIO |
| `EXAMPLE_SD_D0_PIN` | `4` | SDMMC D0 GPIO |

## Requirements

- **ESP-IDF v5.4+** with `CONFIG_MBEDTLS_SSL_DTLS_SRTP=y` (set in `sdkconfig.defaults`)
- FAT32 formatted SD card with H.264 Annex-B frame files

## Expected Serial Output

```
I (xxx) nanortc_video: nanortc ESP32 Video example — H.264 from SD, 25 fps
I (xxx) nanortc_video: SD card mounted: SD (xxxMB)
I (xxx) nanortc_video: Video source: /sd/h264SampleFrames (1500 frames)
I (xxx) nanortc_video: Station IP: 192.168.x.xxx
I (xxx) nanortc_video: HTTP server started on port 80
I (xxx) nanortc_video: Open http://192.168.x.xxx/ in your browser
I (xxx) nanortc_video: WebRTC task started
I (xxx) nanortc_video: Got SDP offer (xxx bytes)
I (xxx) nanortc_video: SDP answer generated (xxx bytes)
I (xxx) nanortc_video: ICE connected
I (xxx) nanortc_video: DTLS connected — starting video
```
