# rk3588_uvc_camera

Real-time H.264 video + Opus audio streaming from a USB UVC camera
(with its built-in USB Audio Class microphone) to one or more browser
viewers, using the RK3588 hardware codec stack, libopus, and the
nanortc Sans I/O WebRTC engine.

Audio is optional and enabled automatically when `libopus` and
`libasound2` are available at configure time and `NANORTC_FEATURE_AUDIO=ON`
is set. If either is missing, the build degrades to video-only without
any source changes.

## Quick start

```
# 1. Dev machine — start signaling server (also serves the browser UI)
python3 examples/browser_interop/signaling_server.py --port 8765 \
        --www-dir examples/rk3588_uvc_camera

# 2. RK3588 device — start camera (auto-discovers signaling on LAN)
rk3588_uvc_camera

# 3. Browser — open http://localhost:8765/ and click Start
```

The camera broadcasts a UDP discovery packet on port 19730 and
automatically connects to the first signaling server that replies. Use
`-s host:port` to skip discovery and connect directly. If the defaults
don't match your camera, run `v4l2-ctl --list-devices` and
`v4l2-ctl -d /dev/videoN --list-formats-ext` to find a node that exposes
MJPG, then override with `-d /dev/videoN -W ... -H ... -b ...`.

## Tested hardware

| Component | Model |
|-----------|-------|
| SoC | Rockchip RK3588 (Orange Pi 5 Ultra) |
| Camera | UGREEN 4K USB UVC (USB 3.0) |
| Encoder | Rockchip MPP H.264 (`mpph264enc`) |
| Decoder | Rockchip MPP JPEG (`mppjpegdec`) |
| RGA driver | librga 1.9.3 |
| Dev machine | macOS (Apple Silicon), Chrome browser |
| Network | Gigabit Ethernet, same LAN |

## Verified configurations

| Resolution | FPS | Bitrate | Backend | CPU (1 viewer) | Status |
|---|---|---|---|---|---|
| 1920x1080 | 30 | 8 Mbps | GStreamer (MJPG path) | low | Verified, smooth playback |
| 2592x1520 | 30 | 12 Mbps | GStreamer (MJPG path) | ~39% of 1 core | Verified end-to-end |
| **3840x2160** | **30** | **20 Mbps** | **GStreamer (MJPG path)** | **~64% of 1 core** | **Default — verified end-to-end (4K30)** |
| Any | Any | Any | FFmpeg (`h264_rkmpp`) | — | **Untested on this distro** — see note below |

CPU figures are the `rk3588_uvc_camera` process `%CPU` as reported by
`ps`, **as a fraction of one core** (so 39% is about 5% of total load
on the 8-core RK3588). The actual JPEG decode and H.264 encode run on
the MPP IP block; userspace cost comes from DMA-BUF shuffling in the
GStreamer plugins plus the per-frame `malloc`+`memcpy` in
`media_queue.c::media_queue_push`. A buffer pool in the media queue
would materially reduce both numbers.

> **FFmpeg backend caveat**: the Orange Pi 5 Ultra Ubuntu image's stock
> ffmpeg does not include `h264_rkmpp` and the libavformat V4L2 path then
> falls back to software `swscale` for YUYV→NV12. That caps throughput at
> ~22 fps for 2592x1520 because it saturates a single core. The
> GStreamer backend is the verified path on this device.

## Build

Requires either FFmpeg or GStreamer development headers on the RK3588
device for video. For audio (optional), `libopus` and `libasound2` must
also be available.

```bash
# GStreamer backend + audio (recommended on RK3588)
cmake -B build \
      -DRK3588_CAPTURE_BACKEND=gstreamer \
      -DNANORTC_FEATURE_VIDEO=ON \
      -DNANORTC_FEATURE_AUDIO=ON \
      -DNANORTC_FEATURE_DATACHANNEL=ON \
      -DNANORTC_CRYPTO=openssl \
      -DNANORTC_BUILD_EXAMPLES=ON
cmake --build build -- rk3588_uvc_camera

# Video-only (omit -DNANORTC_FEATURE_AUDIO=ON)
cmake -B build \
      -DRK3588_CAPTURE_BACKEND=gstreamer \
      -DNANORTC_FEATURE_VIDEO=ON \
      -DNANORTC_FEATURE_DATACHANNEL=ON \
      -DNANORTC_CRYPTO=openssl \
      -DNANORTC_BUILD_EXAMPLES=ON
cmake --build build -- rk3588_uvc_camera

# FFmpeg backend (default if libav* found, but see caveat above)
cmake -B build \
      -DNANORTC_FEATURE_VIDEO=ON \
      -DNANORTC_FEATURE_AUDIO=ON \
      -DNANORTC_FEATURE_DATACHANNEL=ON \
      -DNANORTC_CRYPTO=openssl \
      -DNANORTC_BUILD_EXAMPLES=ON
cmake --build build -- rk3588_uvc_camera
```

CMake prints `rk3588_uvc_camera: audio=on (libopus …, libasound …)` when
audio support is compiled in; if `libopus`/`libasound2` are installed
but `NANORTC_FEATURE_AUDIO=OFF`, it prints a reminder to pass the flag.

### Device dependencies

**GStreamer backend** (`mpph264enc` + `mppjpegdec`):
```
libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
gstreamer1.0-rockchip1   # provides mpph264enc and mppjpegdec
gstreamer1.0-plugins-good # provides jpegparse
```

**FFmpeg backend** (`h264_rkmpp`):
```
libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libavdevice-dev
```
FFmpeg must be compiled with `--enable-rkmpp` for hardware encoding.

**Audio (optional)**:
```
libopus-dev libasound2-dev
```

## Command-line options

```
rk3588_uvc_camera [options]
  -d DEV   V4L2 device        (default /dev/video1)
  -W N     width               (default 3840)
  -H N     height              (default 2160)
  -f N     fps                 (default 30)
  -b N     video bitrate bps   (default 20000000)
  -e ENC   encoder element     (default h264_rkmpp or mpph264enc)
  -s H:P   signaling server    (default: auto-discover on LAN)
  -A DEV   ALSA PCM device     (default plughw:CARD=U4K,DEV=0)   [audio build]
  -R N     audio bitrate bps   (default 64000)                    [audio build]
  -h       show help
```

The audio options (`-A`, `-R`) are only present in binaries built with
`NANORTC_FEATURE_AUDIO=ON` + libopus + libasound2. Audio is always
stereo 48 kHz Opus VOIP — the UGREEN U4K microphone only supports
stereo capture, and the rtpmap is always `opus/48000/2` per RFC 7587.
If the configured ALSA device cannot be opened, the process falls back
to video-only and logs `[audio] start failed, running video-only`.

Override the defaults if you want a different resolution / bitrate. The
default 4K30 path is the camera's highest native MJPG mode. Drop down for
lower CPU or to fit a slower link:
```bash
# 1080p30 at 8 Mbps (lower CPU/bandwidth)
rk3588_uvc_camera -W 1920 -H 1080 -b 8000000

# 2592x1520 at 12 Mbps (sensor crop mode)
rk3588_uvc_camera -W 2592 -H 1520 -b 12000000
```

## Architecture

### Video processing flow

Each box is one stage; the column on the right shows the data format
**leaving** that stage. Hardware blocks run on the RK3588 MPP IP and
move data via DMA-BUF (no userspace copies between them).

```
┌──────────────────────┐
│  USB camera          │  sensor → on-camera ISP → JPEG encoder
│  (UVC, /dev/videoN)  │
└──────────┬───────────┘
           │  MJPG over USB 3.0 bulk transfer
           v                                              ─── format ───
┌──────────────────────┐
│  v4l2src             │  V4L2 mmap buffers (kernel→user)  GstBuffer
└──────────┬───────────┘                                   (raw JPEG)
           v
┌──────────────────────┐
│  jpegparse           │  reads JPEG SOF marker → fills    GstBuffer
│                      │  caps with width/height (req'd    + image/jpeg
│                      │  by mppjpegdec)                   caps
└──────────┬───────────┘
           v
┌──────────────────────┐
│  queue               │  decouples capture thread from    same
│  (4 buf, leaky)      │  decode thread; absorbs jitter
└──────────┬───────────┘
           v
┌══════════════════════┐
║  mppjpegdec    HW    ║  RK3588 MPP JPEG decoder          NV12 in
║                      ║  reads JPEG from DMA-BUF          DMA-BUF
║                      ║  writes NV12 (Y + interleaved UV)
└══════════╤═══════════┘
           v
┌══════════════════════┐
║  mpph264enc    HW    ║  RK3588 MPP H.264 encoder         H.264
║                      ║  reads NV12 from DMA-BUF          Annex-B
║                      ║  baseline / level 5.1 / CBR       (NAL units)
║                      ║  GOP = fps × 2 (2-second IDR)
║                      ║  header-mode=each-idr (SPS/PPS)
└══════════╤═══════════┘
           v
┌──────────────────────┐
│  appsink             │  on_new_sample() callback fires   bytes +
│                      │  on the GStreamer streaming       PTS in ms
│                      │  thread
└──────────┬───────────┘
           │
           │  malloc + memcpy each frame into a queued slot
           │  (dominant userspace CPU cost — buffer pool TODO)
           v
┌──────────────────────┐
│  frame_queue         │  16-slot ring buffer + pipe wake  malloc'd
│  (capture thread)    │  to notify main loop              copy
└══════════╤═══════════┘
═══════════╪═════════════════ thread boundary ════════════════════════
           v
┌──────────────────────┐
│  main loop           │  select() wakes on pipe →
│  (event loop)        │  fq_pop() drains all queued frames
└──────────┬───────────┘
           │  for each connected viewer (up to MAX_SESSIONS = 4):
           v
┌──────────────────────┐
│  nanortc_send_video  │  splits Annex-B → NAL units      RTP packets
│                      │  packetizes per RFC 6184          (~1200 B,
│                      │  (single-NAL or FU-A frag)        ~330/IDR
│                      │                                   at 4K)
└──────────┬───────────┘
           v
┌──────────────────────┐
│  SRTP encrypt        │  AES-CTR + HMAC-SHA1 with         SRTP packets
│  (in nanortc)        │  DTLS-derived keys
└──────────┬───────────┘
           v
┌──────────────────────┐
│  nanortc_poll_output │  yields ready packets to caller   sockaddr +
│                      │                                   payload
└──────────┬───────────┘
           v
┌──────────────────────┐
│  sendto()            │  per-viewer UDP socket
│  (one per session)   │  remote = browser ICE candidate
└──────────┬───────────┘
           v
       ╔════════╗      ╔════════╗      ╔════════╗
       ║Browser1║      ║Browser2║ ...  ║Browser4║   each runs an
       ╚════════╝      ╚════════╝      ╚════════╝   independent
                                                     RTCPeerConnection
```

### Audio processing flow

When built with audio support, a second capture path runs alongside the
video pipeline on its own pthread:

```
┌──────────────────────┐
│  USB mic             │  UGREEN U4K USB Audio Class interface
│  (hw:CARD=U4K,DEV=0) │  S16_LE stereo, 48 kHz only
└──────────┬───────────┘
           │  isochronous USB audio, ~1ms packet interval
           v
┌──────────────────────┐
│  ALSA plug/hw        │  snd_pcm_readi() blocks until a 20 ms period
│  (plughw)            │  (960 frames × 2 ch × 16 bit = 3840 bytes)
└──────────┬───────────┘
           v
┌──────────────────────┐
│  libopus             │  OPUS_APPLICATION_VOIP, stereo, 64 kbps,
│  opus_encode()       │  inband FEC on, loss perc 5%
└──────────┬───────────┘                                 ~120 B / 20 ms
           v
┌──────────────────────┐
│  audio_queue         │  32-slot ring + wake pipe → main loop
│  (capture thread)    │
└══════════╤═══════════┘
═══════════╪═════════════════ thread boundary ════════════════════════
           v
┌──────────────────────┐
│  main loop           │  select() wakes on audio pipe → aq_pop()
└──────────┬───────────┘
           │  broadcast to every connected session_t:
           v
┌──────────────────────┐
│  nanortc_send_audio  │  RTP (PT 111, opus/48000/2 per RFC 7587)
│                      │  → SRTP → per-viewer UDP socket
└──────────────────────┘
```

The capture pthread sleeps inside `snd_pcm_readi()`; shutdown uses
`pthread_kill(audio_tid, SIGUSR1)` to interrupt the read via `EINTR`
(an empty SIGUSR1 handler is installed in the audio thread, and the
main thread blocks SIGUSR1 before spawning the capture thread so the
signal is only delivered to the ALSA reader).

### Components in code

- **`capture_gstreamer.c`** owns the v4l2src→appsink half (camera →
  H.264 Annex-B). Runs on its own GStreamer streaming thread.
- **`audio_capture_alsa.c`** (optional) owns the ALSA → libopus half.
  Runs on its own pthread; emits 20 ms Opus packets via a callback.
- **`media_queue.h/.c`** is the cross-thread bridge from a capture
  thread to the main event loop. A single ring-buffer + wake-pipe
  template, instantiated once per media kind.
- **`media_pipeline.h/.c`** wires capture and audio_capture to the
  two media queues and owns the broadcast-to-sessions loop. The
  event loop in `main.c` hands the pipeline a fd_set plus the
  session array and doesn't need to know which kinds are active.
- **`session.h/.c`** owns one `session_t` per browser viewer: its own
  `nanortc_t` instance, its own UDP socket, its own ICE/DTLS/SRTP state.
  Holds both `video_mid` and `audio_mid`, and centralises the
  track-add order in `add_local_tracks` so the browser's
  `addTransceiver` order and the native side stay aligned.
- **`main.c`** drives the `select()` event loop: registers the
  pipeline's wake pipes, drains to sessions, pumps
  `nanortc_poll_output()`, handles signaling messages and cleanup.

### Signaling

The signaling server runs on the dev machine, not on the device, and
relays SDP offers/answers and ICE candidates over HTTP long-poll.

```
[camera]               [signaling_server.py]               [browser]
    │                          │                              │
    │  POST /join?role=host    │                              │
    ├─────────────────────────►│                              │
    │      {id: 0}             │                              │
    │◄─────────────────────────┤                              │
    │                          │   POST /join (viewer)        │
    │                          │◄─────────────────────────────┤
    │                          │      {id: N}                 │
    │                          ├─────────────────────────────►│
    │                          │   POST /send  offer SDP      │
    │   GET /recv (long-poll)  │◄─────────────────────────────┤
    │  offer SDP {from: N}     │                              │
    │◄─────────────────────────┤                              │
    │  POST /send?to=N answer  │                              │
    ├─────────────────────────►│   GET /recv (long-poll)      │
    │                          │  answer SDP                  │
    │                          ├─────────────────────────────►│
    │  ICE candidates exchanged in both directions...         │
    │                                                          │
    │  ════════ direct UDP RTP/SRTP after ICE+DTLS ════════    │
    │ ◄───────────────────────────────────────────────────────►│
```

### Capture backends

Both backends implement the same `capture.h` interface:

- **GStreamer** (`capture_gstreamer.c`, recommended): Builds the
  full-hardware pipeline shown above. The camera produces MJPG, the
  RK3588 MPP decoder unpacks it to NV12 in DMA-BUF, and the MPP H.264
  encoder consumes that NV12 directly. No CPU colorspace conversion;
  no buffer copies between elements. The `jpegparse` element is
  required so that `mppjpegdec` learns the image dimensions from the
  JPEG SOF marker — without it, `v4l2src` reports `stream error -5`
  silently and the pipeline never produces a frame.

- **FFmpeg** (`capture_ffmpeg.c`): Uses `libavdevice` for V4L2 capture,
  `libswscale` for YUYV→NV12 conversion, and `libavcodec` with
  `h264_rkmpp` for hardware H.264 encoding. Requires an ffmpeg build
  with `--enable-rkmpp`. On stock distro ffmpeg the rkmpp encoder is
  usually missing and the pipeline degrades to software encoding plus
  swscale, which caps at ~22 fps for 2592x1520.

Selected at build time via `-DRK3588_CAPTURE_BACKEND=ffmpeg|gstreamer`.

### Diagnostic stats

Every 5 seconds the app prints a stats line. Sample from a verified
4K30 run on Orange Pi 5 Ultra:

```
[stats] 151 frames ~19919 kbps 1 viewer(s) | PLI=0 drop=0 IDR_max=350KB | rtp_sent=99002 rtt=0ms bwe=300kbps
```

| Field | Meaning | Action if abnormal |
|-------|---------|-------------------|
| `frames` | Encoded frames sent in this interval | Low → encoder stalled or capture queue dropping |
| `kbps` | Actual bitrate sent on the wire | Much lower than `-b` → encoder under-producing |
| `PLI=N` | Keyframe requests from browser | >0 → packet loss exceeding NACK recovery |
| `drop=N` | Frames dropped from `frame_queue` | >0 → main loop too slow to drain |
| `IDR_max` | Largest keyframe in interval | Approaching `NANORTC_OUT_QUEUE_SIZE × MTU` (~600 KB) → bump the queue |
| `rtt` | RTT from RTCP | See caveat below |
| `bwe` | Browser bandwidth estimate | See caveat below |

> **Known caveat — `rtt=0ms` and `bwe=300kbps` look stuck.** This is
> not a camera-side problem. nanortc currently parses only REMB
> (`draft-alvestrand-rmcat-remb-03`) feedback for bandwidth estimation,
> but Chrome sends `transport-cc` instead. The displayed BWE therefore
> stays at the initial value (`NANORTC_BWE_INITIAL_BITRATE`, 300 kbps)
> and `rtt` stays at 0 ms for the lifetime of the session. The actual
> RTP throughput and quality are unaffected — verified by `kbps` and
> `PLI=0` in the same line. Tracked as a nanortc-library limitation;
> not actionable inside this example.

## Troubleshooting

- **`RgaBlit RGA_BLIT fail: Invalid argument` / `10000 is unsupport format now`**
  in stderr — `mpph264enc` was given an input format its underlying RGA
  backend can't handle (for example, raw YUY2 with librga 1.9.3). Route
  capture through `mppjpegdec` from MJPG instead. The default GStreamer
  pipeline already does this; the error means something rewrote the
  pipeline back to a YUY2 source.

- **`v4l2src: Internal data stream error (reason error -5)`** when
  feeding `mppjpegdec` — `jpegparse` is missing from the pipeline.
  `mppjpegdec` reads dimensions from the JPEG SOF marker, not from
  GStreamer caps; without `jpegparse` between `v4l2src` and
  `mppjpegdec`, capture appears to start, then fails silently.

- **`bwe=300kbps`, `rtt=0ms` permanently** — see the diagnostic-stats
  caveat above. nanortc-library limitation, not a camera bug.

- **Single-thread CPU spike at 4K** — encoded H.264 frames flow
  through `media_queue.c::media_queue_push`, which `malloc`s and
  `memcpy`s every frame. At 4K30 / 20 Mbps that is ~50 KB × 30 fps +
  occasional ~400 KB IDR bursts. Correct but allocator-heavy; a
  buffer pool is the obvious optimization but is out of scope for
  this example.

- **`Join as host failed (status=409)` when restarting the camera** —
  the signaling server's host slot (peer 0) is sticky in host mode.
  `/leave?id=0` only drains the host's message queue; it does not
  release the slot. Workaround: restart `signaling_server.py`.

- **Wrong v4l2 device node** — the RK3588 ISP typically claims
  `/dev/video0`; the USB UVC camera ends up on `/dev/video1` or higher.
  Run `v4l2-ctl --list-devices` to identify the camera, then
  `v4l2-ctl -d /dev/videoN --list-formats-ext` to confirm it exposes
  MJPG (which the GStreamer pipeline requires). Override with
  `-d /dev/videoN`.

- **No audio in the browser** — check the startup log. If it says
  `audio=off` the binary was built without audio; rebuild with
  `-DNANORTC_FEATURE_AUDIO=ON` and make sure `libopus-dev` /
  `libasound2-dev` are installed. If it says `audio=on` but
  `[audio] start failed, running video-only`, the ALSA device name is
  wrong — run `arecord -L | grep -i card` on the device to find the
  correct `plughw:CARD=…,DEV=…` string and pass it via `-A`. If the
  startup line says `audio=on` and no error follows but you still
  hear nothing in the browser, check Chrome's tab mute state and
  `chrome://webrtc-internals` for the `RTCInboundRtpAudioStream`
  `packetsReceived` counter.

## File overview

| File | Description |
|------|-------------|
| `main.c` | Event loop, signaling thread, CLI |
| `media_queue.h/.c` | Thread-safe encoded media frame queue (one instance per kind) |
| `media_pipeline.h/.c` | Wires capture + audio_capture to queues; drains queues to all sessions |
| `sig_queue.h/.c` | Thread-safe signaling message queue (sig thread → main) |
| `session.h/.c` | Per-viewer session lifecycle, IP enumeration, output dispatch |
| `capture.h` | Backend-agnostic video capture + encode interface |
| `capture_ffmpeg.c` | FFmpeg/libav* video backend (`h264_rkmpp` if available) |
| `capture_gstreamer.c` | GStreamer video backend (MJPG → `mppjpegdec` → `mpph264enc`) |
| `audio_capture.h` | Backend-agnostic audio capture + encode interface (audio build) |
| `audio_capture_alsa.c` | ALSA + libopus microphone pipeline (audio build) |
| `index.html` | Browser viewer (WebRTC offerer, recvonly video + audio) |
| `nanortc_app_config.h` | Overrides `NANORTC_OUT_QUEUE_SIZE=512` for HD/4K IDR fragmentation |
| `CMakeLists.txt` | Dual video backend + optional audio build configuration |
