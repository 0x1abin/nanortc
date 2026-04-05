# macos_camera - macOS Camera/Mic to Browser

Real-time macOS camera and microphone streaming to browser via nanortc WebRTC.
Supports multiple simultaneous viewers and reconnection.

## Architecture

```
+-----------------+       HTTP signaling        +------------------+
|  Browser A      | <-- (signaling_server.py) ->|   macos_camera   |
|  Browser B      |      localhost:8765         |   (host, pid=0)  |
|  ...            |                             |                  |
+-----------------+                             +--------+---------+
        |                                                |
        +---------- UDP (STUN / DTLS / SRTP) ------------+
                      (per-viewer UDP port)

macos_camera internals:
  AVFoundation ─→ VideoToolbox H.264 ─→ ┐
  AVFoundation ─→ Opus (libopus)     ─→ ├→ frame_queue ─→ nanortc sessions
                                         │    (broadcast to all viewers)
                                         └→ pipe wakes select()
```

- **signaling_server.py** — HTTP relay with host mode (peer 0 = host, peers 1..N = viewers).
- **macos_camera** — Captures camera/mic, encodes H.264+Opus, manages multiple nanortc sessions.
- **index.html** — Browser UI (reused from browser_interop), served at `http://localhost:8765/`.

## Files

| File | Description |
|------|-------------|
| `main.m` | Entry point, Opus encoding, multi-session event loop |
| `av_capture.h/m` | AVFoundation camera + microphone capture |
| `vt_encoder.h/m` | VideoToolbox H.264 hardware encoder (Annex-B output) |
| `nanortc_app_config.h` | nanortc config overrides (output queue size for HD video) |
| `CMakeLists.txt` | macOS-only build rules |

## Dependencies

- macOS 11.0+ (AVFoundation, VideoToolbox)
- OpenSSL (`brew install openssl`)
- libopus (`brew install opus`)
- Python 3 (for signaling server)

## Build

```bash
cmake -B build \
    -DNANORTC_CRYPTO=openssl \
    -DNANORTC_BUILD_EXAMPLES=ON \
    -DNANORTC_FEATURE_AUDIO=ON \
    -DNANORTC_FEATURE_VIDEO=ON
cmake --build build -j$(sysctl -n hw.ncpu)
```

## Run

### Step 1: Start signaling server

```bash
python3 examples/browser_interop/signaling_server.py --port 8765
```

### Step 2: Start macos_camera

```bash
./build/examples/macos_camera/macos_camera -s localhost:8765
```

On first run, macOS will prompt for camera and microphone permissions.

### Step 3: Open browser

Navigate to `http://localhost:8765/`, select **Offerer**, click **Connect**.

Multiple browser tabs can connect simultaneously (up to 4 viewers).

### CLI Options

```
  -b IP          Bind/candidate IP (default: auto-detect)
  -s HOST:PORT   Signaling server (default: localhost:8765)
```

## Configuration

Video and audio parameters are defined as constants in `main.m`:

```c
#define VIDEO_WIDTH        1280    // 1280 (720p) or 1920 (1080p)
#define VIDEO_HEIGHT       720
#define VIDEO_FPS          30
#define VIDEO_BITRATE_KBPS 3000
#define VIDEO_KEYFRAME_S   2

#define OPUS_SAMPLE_RATE   48000
#define OPUS_CHANNELS      1
#define OPUS_BITRATE       64000

#define MAX_SESSIONS       4      // max simultaneous viewers
```

For higher resolutions (1080p+), the nanortc output queue is enlarged via
`nanortc_app_config.h` using the `NANORTC_CONFIG_FILE` mechanism.

## Expected Output

```
[opus] Encoder ready (48kHz mono, 64000 bps)
[vt_encoder] H.264 encoder ready (1280x720@30fps, 3000kbps, KF every 2s)
[av_capture] Capture started (video=1280x720@30fps, audio=48000Hz/1ch)
[sig] Joined as host (peer 0, server localhost:8765)
macos_camera (host mode, ip=192.168.1.100, sig=localhost:8765, max_viewers=4)
Waiting for viewers (Ctrl+C to stop)...
[sig] Got offer from viewer 1 (1234 bytes)
[session] Created session for viewer 1 (udp=192.168.1.100:51234)
[session 1] ICE connected
[session 1] Media connected — forcing keyframe
```

## Signaling Protocol (Host Mode)

The signaling server supports a host mode extension for multi-viewer streaming:

| Endpoint | Description |
|----------|-------------|
| `POST /join?role=host` | Register as host (pid=0), enables multi-viewer |
| `POST /join` | Register as viewer (pid=1,2,...,N) |
| `POST /send?id=N` | Viewer sends to host (server injects `"from": N`) |
| `POST /send?id=0&to=M` | Host sends to specific viewer M |

Legacy 2-peer mode is preserved when no host is registered.

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| No video in browser | Camera permission denied | System Settings → Privacy → Camera → allow Terminal |
| Black screen for a few seconds | Viewer connected between keyframes | Fixed: keyframe is forced on connect |
| Video glitches at high bitrate | Output queue too small | Increase `NANORTC_OUT_QUEUE_SIZE` in `nanortc_app_config.h` |
| `Join as host failed` | Signaling server not running | Start `signaling_server.py` first |
| `No free slots` | 4 viewers already connected | Close a browser tab or increase `MAX_SESSIONS` |
