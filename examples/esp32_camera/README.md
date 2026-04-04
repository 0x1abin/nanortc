# ESP32-P4 Camera — Live WebRTC Streaming

[中文文档 (Chinese)](README_CN.md)

ESP32-P4 captures video from a MIPI CSI camera and audio from an ES8311
codec, encodes H.264 + Opus, and streams both to a browser in real-time
using NanoRTC WebRTC.

## Hardware

| Item | Spec |
|------|------|
| Board | ESP32-P4-Nano v1.0 (or any ESP32-P4 board with CSI camera + I2S codec) |
| SoC | ESP32-P4, dual-core RISC-V @ 360 MHz, HW H.264 encoder |
| PSRAM | Required (32 MB recommended) |
| Network | Ethernet (ESP32-P4 has no WiFi) |
| Camera | OV5647 (MIPI CSI) — configurable via board YAML |
| Audio | ES8311 codec (I2S) — configurable via board YAML |

Hardware pin assignments are defined in `boards/<board>/board_peripherals.yaml`,
not hardcoded in application code. See [Custom Board](#custom-board) to adapt
to your own hardware.

## Build & Flash

```bash
cd examples/esp32_camera

# 1. Set target (downloads managed components)
idf.py set-target esp32p4

# 2. Generate board config (first time or when switching boards)
python managed_components/espressif__esp_board_manager/gen_bmgr_config_codes.py \
  -b esp32_p4_nano -c boards

# 3. Build and flash
idf.py build
idf.py flash monitor
```

After step 2, subsequent builds only need `idf.py build`.

## Usage

1. Wait for the device to obtain an IP address (printed on serial console)
2. Open `http://<device-ip>/` in a browser (Chrome recommended)
3. Click **Connect**
4. Live video + audio will stream to the page

## Architecture

```
Core 1 — camera_task (pri 6)               Core 0 — webrtc_task (pri 5)
┌─────────────────────────────┐            ┌─────────────────────────────┐
│ V4L2 DQBUF (YUV420)         │            │ nano_run_loop_step()        │
│         ↓                   │  FreeRTOS  │   ↑ DTLS/STUN/RTP polling   │
│ H.264 HW encode             │───Queue───→│ nanortc_send_video()        │
│         ↓                   │  (depth=2) │ nanortc_send_audio()        │
│ V4L2 QBUF (return buffer)   │            │   ↓ RTP/SRTP → UDP          │
└─────────────────────────────┘            └─────────────────────────────┘

Core 0 — microphone_task (pri 7)
┌──────────────────────────────┐
│ esp_capture (ES8311 → Opus)  │───Queue (depth=4)───→ webrtc_task
└──────────────────────────────┘
```

- **Board init**: `esp_board_manager` handles I2C, I2S, XCLK, LDO, codec, and camera sensor
- **Camera**: V4L2 API via board manager's `dev_path`, MMAP double buffering
- **H.264**: Hardware encoder (ESP32-P4 only), Annex-B output, dynamic keyframe/bitrate
- **Audio**: `esp_capture` framework with Opus encoding, codec from board manager
- **WebRTC**: NanoRTC (Sans I/O) + mbedTLS (DTLS-SRTP)
- **Signaling**: HTTP POST `/offer` (SDP offer/answer)

## Custom Board

Application code uses `esp_board_manager` handles — **no pin numbers or chip
names are hardcoded**. To adapt to a different ESP32-P4 board:

1. Create `boards/my_board/` with 4 files (copy from `boards/esp32_p4_nano/`):

   | File | Purpose |
   |------|---------|
   | `board_info.yaml` | Board name, chip type |
   | `board_peripherals.yaml` | I2C / I2S / GPIO / LDO pin assignments |
   | `board_devices.yaml` | Camera, audio codec, SD card device config |
   | `sdkconfig.defaults.board` | Camera sensor driver config (e.g. OV5647, SC2336) |

2. Generate and build:
   ```bash
   python managed_components/espressif__esp_board_manager/gen_bmgr_config_codes.py \
     -b my_board -c boards
   idf.py build
   ```

### What to change in YAML

| If your board has... | Modify in... |
|---------------------|--------------|
| Different I2C/I2S pins | `board_peripherals.yaml` |
| Different camera sensor | `board_devices.yaml` (sub_type, xclk) + `sdkconfig.defaults.board` |
| Different audio codec (ES8388, etc.) | `board_devices.yaml` (chip field) |
| No PA amplifier | Remove `gpio_pa_control` from peripherals and device refs |
| DVP camera instead of CSI | Change `sub_type: "dvp"`, remove `ldo_mipi` |

### Constraints

- **ESP32-P4 only**: Hardware H.264 encoder is P4-specific
- **PSRAM required**: Opus encoder needs 40 KB+ stack, camera buffers use PSRAM
- **CSI cameras need LDO_MIPI**: `main.c` calls `esp_board_periph_init(LDO_MIPI)` — comment this out for DVP/SPI cameras

## Configuration

Adjustable via `idf.py menuconfig` → "ESP32 Camera Example":

| Parameter | Default | Description |
|-----------|---------|-------------|
| `EXAMPLE_UDP_PORT` | 9999 | WebRTC STUN/DTLS/RTP port |
| `EXAMPLE_VIDEO_WIDTH` | 1280 | Video width (must match sensor mode) |
| `EXAMPLE_VIDEO_HEIGHT` | 960 | Video height (must match sensor mode) |
| `EXAMPLE_VIDEO_FPS` | 30 | Video frame rate |
| `EXAMPLE_H264_BITRATE_KBPS` | 1024 | H.264 target bitrate (kbps) |
| `EXAMPLE_H264_GOP` | 60 | Keyframe interval (GOP=60 at 30fps = 2s) |

The ESP32-P4-Nano board overrides resolution to 1920x1080 in `sdkconfig.defaults.esp32p4`.

## Keyframe Handling

- **Periodic IDR**: Every GOP frames
- **IDR on connect**: Forced immediately on WebRTC connection
- **PLI response**: Browser RTCP PLI → `NANORTC_EV_KEYFRAME_REQUEST` → encoder IDR

## File Structure

```
esp32_camera/
├── CMakeLists.txt                  # Project config (includes board_manager.defaults)
├── partitions.csv                  # Flash partition table
├── sdkconfig.defaults              # Common config (mbedTLS, lwIP, nanortc)
├── sdkconfig.defaults.esp32p4      # P4-specific (SPIRAM, Ethernet, resolution)
├── boards/
│   └── esp32_p4_nano/              # Board config (YAML, see Custom Board section)
│       ├── board_info.yaml
│       ├── board_peripherals.yaml
│       ├── board_devices.yaml
│       └── sdkconfig.defaults.board
├── components/
│   └── gen_bmgr_codes/             # Generated by gen_bmgr_config_codes.py (not in git)
├── main/
│   ├── CMakeLists.txt
│   ├── Kconfig.projbuild           # Menuconfig: video/encoder params
│   ├── idf_component.yml           # Dependencies: esp_board_manager, esp_capture, etc.
│   ├── main.c                      # Board init, WebRTC session, task orchestration
│   ├── camera.c / camera.h         # V4L2 capture (uses board manager dev_path)
│   ├── encoder.c / encoder.h       # H.264 HW encoder (keyframe + bitrate control)
│   ├── microphone.c / microphone.h # Opus capture (uses board manager codec_dev)
│   └── index.html                  # Browser WebRTC UI
├── README.md
└── README_CN.md
```

## Known Limitations

- Ethernet only (ESP32-P4 has no WiFi)
- Single client connection at a time
- `VIDIOC_S_PARM` frame rate not effective in current esp_video; sensor default is used
