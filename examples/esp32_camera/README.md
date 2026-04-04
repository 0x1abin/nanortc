# ESP32-P4 Camera — Live WebRTC Streaming

ESP32-P4 通过 MIPI CSI 采集 OV5647 摄像头画面，经硬件 H.264 编码后，
使用 NanoRTC 以 WebRTC 协议实时推送到浏览器。

## Hardware

### Tested Board

| Item | Spec |
|------|------|
| SoC | ESP32-P4 (rev v1.0) |
| Board | ESP32-P4 Nano |
| Flash | 8 MB QIO |
| PSRAM | 32 MB (200 MHz) |
| Network | Internal EMAC + IP101 PHY (Ethernet) |
| Camera | OV5647 (5MP, MIPI CSI 2-lane) |

### GPIO Assignment

| Function | GPIO | Note |
|----------|------|------|
| Camera XCLK | 33 | 24 MHz clock output |
| Camera I2C SCL | 8 | SCCB control bus |
| Camera I2C SDA | 7 | SCCB control bus |
| Camera Reset | -1 | Not connected |
| Camera Power Down | -1 | Not connected |
| Ethernet MDC | 31 | EMAC management clock |
| Ethernet MDIO | 52 | EMAC management data |
| Ethernet PHY Reset | -1 | Not connected |
| UART TX | 38 | Console output |
| UART RX | 37 | Console input |

GPIO 引脚可通过 `idf.py menuconfig` → "ESP32 Camera Example" 修改。

### Camera Sensor Modes (OV5647)

| Resolution | Format | FPS | Kconfig Option |
|------------|--------|-----|----------------|
| 1920x1080 | RAW10 | 30 | `CAMERA_OV5647_MIPI_RAW10_1920X1080_30FPS` (default) |
| 1280x960 | RAW10 | 45 | `CAMERA_OV5647_MIPI_RAW10_1280X960_BINNING_45FPS` |
| 800x800 | RAW8 | 50 | `CAMERA_OV5647_MIPI_RAW8_800X800_50FPS` |
| 800x640 | RAW8 | 50 | `CAMERA_OV5647_MIPI_RAW8_800X640_50FPS` |
| 800x1280 | RAW8 | 50 | `CAMERA_OV5647_MIPI_RAW8_800X1280_50FPS` |

切换分辨率时需同步修改 `EXAMPLE_VIDEO_WIDTH` 和 `EXAMPLE_VIDEO_HEIGHT`。

## Architecture

```
Core 1 — camera_task (priority 6)          Core 0 — webrtc_task (priority 5)
┌─────────────────────────────┐            ┌─────────────────────────────┐
│ V4L2 DQBUF (YUV420)         │            │ nano_run_loop_step()        │
│         ↓                   │            │   ↑ DTLS/STUN/RTP polling   │
│ H.264 HW encode             │  FreeRTOS  │         ↓                   │
│         ↓                   │───Queue───→│ nanortc_send_video()        │
│ V4L2 QBUF (return buffer)   │  (depth=2) │   ↓ RTP/SRTP → UDP          │
└─────────────────────────────┘            └─────────────────────────────┘
```

- **Camera capture**: esp_video V4L2 API (`/dev/video0`), MMAP double buffering
- **H.264 encoder**: esp_h264 hardware encoder, Annex-B output
- **WebRTC**: NanoRTC (Sans I/O) + mbedTLS (DTLS-SRTP)
- **Signaling**: HTTP POST `/offer` (SDP offer/answer exchange)

## Build & Flash

```bash
cd examples/esp32_camera
idf.py set-target esp32p4
idf.py build
idf.py flash monitor
```

## Usage

1. 烧录后等待设备获取 IP 地址（串口日志会打印）
2. 在浏览器打开 `http://<device-ip>/`
3. 点击 **Connect** 按钮
4. 视频流将在页面上实时显示，点击视频控制栏的全屏按钮可全屏观看

## Configuration

通过 `idf.py menuconfig` → "ESP32 Camera Example" 调整参数：

| Parameter | Default | Description |
|-----------|---------|-------------|
| `EXAMPLE_UDP_PORT` | 9999 | WebRTC STUN/DTLS/RTP 端口 |
| `EXAMPLE_VIDEO_WIDTH` | 1920 | 视频宽度（需匹配传感器模式） |
| `EXAMPLE_VIDEO_HEIGHT` | 1080 | 视频高度（需匹配传感器模式） |
| `EXAMPLE_VIDEO_FPS` | 30 | 视频帧率 |
| `EXAMPLE_H264_BITRATE_KBPS` | 3072 | H.264 目标码率 (kbps) |
| `EXAMPLE_H264_GOP` | 60 | 关键帧间隔（帧数），30fps 下 GOP=60 = 2 秒 |

## Keyframe Handling

- **定时 IDR**: 编码器每 GOP 帧自动生成一个 IDR 关键帧
- **连接时强制 IDR**: WebRTC 连接建立后立即强制编码器输出 IDR，避免浏览器等待下一个 GOP 边界
- **PLI 响应**: 浏览器发送 RTCP PLI 时，nanortc 触发 `NANORTC_EV_KEYFRAME_REQUEST` 事件，编码器在下一帧强制 IDR

## File Structure

```
esp32_camera/
├── CMakeLists.txt              # ESP-IDF project config
├── partitions.csv              # Flash partition table (factory 1984 KB)
├── sdkconfig.defaults          # Common SDK config (mbedTLS, lwIP, nanortc)
├── sdkconfig.defaults.esp32p4  # P4-specific config (SPIRAM, Ethernet, OV5647)
├── main/
│   ├── CMakeLists.txt          # Component sources and embedded files
│   ├── Kconfig.projbuild       # Menuconfig options
│   ├── idf_component.yml       # Managed components (esp_video, esp_cam_sensor)
│   ├── main.c                  # App entry, WebRTC session, dual-core tasks
│   ├── camera.c                # V4L2 camera capture (esp_video)
│   ├── camera.h                # Camera API
│   ├── encoder.c               # H.264 HW encoder (esp_h264)
│   ├── encoder.h               # Encoder API
│   └── index.html              # Browser WebRTC UI
└── README.md
```
