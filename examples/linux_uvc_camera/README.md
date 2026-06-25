# linux_uvc_camera — USB camera → browser, adaptive bitrate

Captures a USB UVC camera on a Linux box, encodes the video, and streams it to
one or more browser viewers over WebRTC using [nanortc](../../). One binary
covers every encoder backend — pick it at runtime with `-e`. The send bitrate
adapts to the network: nanortc's bandwidth estimator (REMB + TWCC loss) feeds a
coordinator that retargets the encoder so playback stays real-time. Microphone
audio (ALSA → Opus) is an optional add-on.

## Encoders (`-e NAME`)

| `-e`          | Backend                  | Codec | Needs                                              |
|---------------|--------------------------|-------|----------------------------------------------------|
| `libx264`     | ffmpeg (software)        | H.264 | nothing — universal default, runs on any machine   |
| `h264_nvenc`  | ffmpeg + NVIDIA          | H.264 | NVIDIA GPU + ffmpeg `--enable-nvenc`               |
| `hevc_nvenc`  | ffmpeg + NVIDIA          | H.265 | NVIDIA GPU + library built with `H265=ON`          |
| `h264_rkmpp`  | ffmpeg + Rockchip MPP    | H.264 | Rockchip SoC w/ MPP (RK356x/RK3576/RK3588) + ffmpeg |
| `mpph264enc`  | GStreamer + Rockchip MPP | H.264 | Rockchip MPP + `-DCAPTURE_BACKEND=gstreamer`       |

Unknown ffmpeg encoder names fall back to `libx264`.

## Build

Default (software H.264, any Linux machine):

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DNANORTC_BUILD_EXAMPLES=ON -DNANORTC_FEATURE_VIDEO=ON
cmake --build build --target linux_uvc_camera -j$(nproc)
```

NVIDIA H.265 (own build dir; enables HEVC in the library):

```bash
cmake -B build-nvenc -DNANORTC_CRYPTO=openssl -DNANORTC_BUILD_EXAMPLES=ON \
      -DNANORTC_FEATURE_VIDEO=ON -DNANORTC_FEATURE_H265=ON
cmake --build build-nvenc --target linux_uvc_camera
```

Optional microphone audio: add `-DNANORTC_FEATURE_AUDIO=ON` and install
`libopus` + `libasound` (ALSA). The build auto-detects them.

## Run

```bash
# 1. signaling server (serves index.html too) — on the box or a dev machine
python3 ../browser_interop/signaling_server.py --port 8765 --www-dir .

# 2. the camera publisher (auto-discovers the signaling server on the LAN)
./build/examples/linux_uvc_camera/linux_uvc_camera -e libx264 -d /dev/video0
#   NVIDIA:  -e hevc_nvenc        Rockchip:  -e h264_rkmpp      with audio: -A default

# 3. open http://<box-ip>:8765/ in Chrome (>=136) or Safari
```

Run `linux_uvc_camera -h` for the full option list (`-W/-H/-f` geometry,
`-b/-m/-M` bitrate envelope, `-i` input format, `-I` single host candidate,
`-A/-R` audio). The page shows a live WebRTC stats panel.

## Layout

| File                              | Role                                                   |
|-----------------------------------|--------------------------------------------------------|
| `main.c`                          | CLI, session pool, event loop, BWE→encoder retargeting |
| `capture_ffmpeg.c`                | FFmpeg capture/encode (libx264 / nvenc / rkmpp)        |
| `capture_gstreamer.c`             | GStreamer capture/encode (Rockchip MPP, opt-in)        |
| `audio_capture_alsa.c`            | ALSA → Opus microphone capture (opt-in)                |
| `media_pipeline.c` / `media_queue.c` | Capture→session broadcast plumbing                  |
| `sig_queue.c`                     | Thread-safe signaling message queue                    |
| `index.html`                      | Browser viewer + live stats                            |

Shared helpers (`run_loop_linux`, `http_signaling`, `sig_discovery`,
`multi_session`, `bwe_coordinator`) live in [`../common/`](../common/).
