# rk3588_uvc_camera

Real-time H.264 video streaming from a USB UVC camera to the browser,
using the RK3588 hardware encoder and the nanortc Sans I/O WebRTC engine.

## Quick start

```
# 1. Dev machine — start signaling server (serves the browser UI)
python3 examples/browser_interop/signaling_server.py --port 8765 \
        --www-dir examples/rk3588_uvc_camera

# 2. RK3588 device — start camera (auto-discovers signaling on LAN)
rk3588_uvc_camera -d /dev/video2

# 3. Browser — open http://localhost:8765/ and click Start
```

The camera broadcasts a UDP discovery packet on port 19730 and
automatically connects to the first signaling server that replies.
Use `-s host:port` to skip discovery and connect directly.

## Tested hardware

| Component | Model |
|-----------|-------|
| SoC | Rockchip RK3588 (Orange Pi 5 Ultra) |
| Camera | UGREEN 4K USB UVC (USB 3.0) |
| Encoder | Rockchip MPP H.264 (`h264_rkmpp` / `mpph264enc`) |
| Dev machine | macOS (Apple Silicon), Chrome browser |
| Network | Gigabit Ethernet, same LAN |

## Supported resolutions

The camera and encoder support up to 4K. Tested configurations:

| Resolution | FPS | Bitrate | Backend | Status |
|------------|-----|---------|---------|--------|
| 1920x1080 | 30 | 8 Mbps | FFmpeg (`h264_rkmpp`) | Verified, smooth playback |
| 1920x1080 | 30 | 8 Mbps | GStreamer (`mpph264enc`) | Verified, smooth playback |
| 3840x2160 | 30 | 20 Mbps | FFmpeg (`h264_rkmpp`) | Encoding verified, needs swscale optimization for full pipeline |

## Build

Requires either FFmpeg or GStreamer development headers on the RK3588 device.

```bash
# FFmpeg backend (default if libav* found)
cmake -B build \
      -DNANORTC_FEATURE_VIDEO=ON \
      -DNANORTC_FEATURE_DATACHANNEL=ON \
      -DNANORTC_CRYPTO=openssl \
      -DNANORTC_BUILD_EXAMPLES=ON
cmake --build build -- rk3588_uvc_camera

# Explicitly select GStreamer backend
cmake -B build \
      -DRK3588_CAPTURE_BACKEND=gstreamer \
      -DNANORTC_FEATURE_VIDEO=ON \
      -DNANORTC_FEATURE_DATACHANNEL=ON \
      -DNANORTC_CRYPTO=openssl \
      -DNANORTC_BUILD_EXAMPLES=ON
cmake --build build -- rk3588_uvc_camera
```

### Device dependencies

**FFmpeg backend** (`h264_rkmpp`):
```
libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libavdevice-dev
```
FFmpeg must be compiled with `--enable-rkmpp` for hardware encoding.

**GStreamer backend** (`mpph264enc`):
```
libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
```
Plus the `gstreamer-rockchip` plugin package for `mpph264enc`.

## Command-line options

```
rk3588_uvc_camera [options]
  -d DEV   V4L2 device        (default /dev/video2)
  -W N     width               (default 1920)
  -H N     height              (default 1080)
  -f N     fps                 (default 30)
  -b N     bitrate in bps      (default 8000000)
  -e ENC   encoder element     (default h264_rkmpp or mpph264enc)
  -s H:P   signaling server    (default: auto-discover on LAN)
  -h       show help
```

## Architecture

```
USB Camera (/dev/videoN)
    |
    v
[capture backend]  ← capture_ffmpeg.c or capture_gstreamer.c
    | YUYV → NV12 → H.264 Annex-B (hardware encoder)
    v
[frame queue]  ← thread-safe ring buffer (capture thread → main thread)
    |
    v
[nanortc]  ← Sans I/O WebRTC engine
    | RTP/SRTP packetization, ICE, DTLS
    v
[UDP socket]  → Browser (Chrome/Safari/Firefox)
    |
    v
[signaling]  ← HTTP long-poll via signaling_server.py
```

### Capture backends

Both backends implement the same `capture.h` interface:

- **FFmpeg** (`capture_ffmpeg.c`): Uses `libavdevice` for V4L2 capture,
  `libswscale` for YUYV→NV12 conversion, and `libavcodec` with the
  `h264_rkmpp` encoder for hardware H.264. Runs capture + encode on a
  dedicated pthread.

- **GStreamer** (`capture_gstreamer.c`): Builds a GStreamer pipeline
  (`v4l2src ! videoconvert ! mpph264enc ! appsink`). GStreamer manages
  its own streaming thread; encoded frames arrive via the appsink callback.

Selected at build time via `-DRK3588_CAPTURE_BACKEND=ffmpeg|gstreamer`.

### Signaling flow

The signaling server runs on the dev machine (not the device):

1. Camera app discovers the signaling server via UDP broadcast (port 19730)
2. Camera joins as **host** (peer 0) via `POST /join?role=host`
3. Browser joins as **viewer** (peer 1+) via `POST /join`
4. Browser creates WebRTC offer → signaling relays to host
5. Host generates answer → signaling relays to browser
6. ICE connectivity checks establish a direct UDP path
7. DTLS handshake → SRTP keys derived
8. H.264 RTP stream begins

### Diagnostic stats

Every 5 seconds the app prints a stats line:

```
[stats] 150 frames ~8012 kbps 1 viewer(s) | PLI=0 drop=0 IDR_max=183KB | rtp_sent=4520 rtt=3ms bwe=9200kbps
```

| Field | Meaning | Action if abnormal |
|-------|---------|-------------------|
| `frames` | Encoded frames in this interval | Low → encoder stalled or camera disconnected |
| `kbps` | Actual bitrate sent | Much lower than `-b` → encoder under-producing |
| `PLI=N` | Keyframe requests from browser | >0 → packet loss exceeding NACK recovery |
| `drop=N` | Frames dropped from queue | >0 → main loop too slow to drain |
| `IDR_max` | Largest keyframe in interval | >200KB → risk of burst loss at 1080p |
| `rtt` | Round-trip time (RTCP) | >100ms → high latency path |
| `bwe` | Browser's bandwidth estimate | Much lower than bitrate → network bottleneck |

## File overview

| File | Description |
|------|-------------|
| `main.c` | Multi-viewer event loop, signaling thread, session management |
| `capture.h` | Backend-agnostic capture + encode interface |
| `capture_ffmpeg.c` | FFmpeg/libav* backend (h264_rkmpp) |
| `capture_gstreamer.c` | GStreamer backend (mpph264enc) |
| `index.html` | Browser viewer (WebRTC offerer, recvonly H.264) |
| `nanortc_app_config.h` | Overrides `NANORTC_OUT_QUEUE_SIZE` to 512 for HD/4K |
| `CMakeLists.txt` | Dual-backend build configuration |
