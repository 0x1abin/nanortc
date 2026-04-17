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
[stats 1] fps=30.0 send=2980 kbps est=3000 kbps loss=0.0% rtt=12 ms
[session 1] BWE DOWN via TWCC: 1800 kbps (was 3000 kbps)
[bwe] encoder target → 1800 kbps (min of 1 viewer)
[stats 1] fps=30.0 send=1760 kbps est=1800 kbps loss=7.8% rtt=35 ms
```

## Adaptive Bitrate (Phase 9)

The example ties the nanortc BWE signal path to the VideoToolbox hardware
encoder so the sending bitrate tracks live network conditions:

- **Bounds**: each session advertises `[500 kbps, 5 Mbps]` to BWE via
  `nanortc_set_bitrate_bounds()` (see `BWE_MIN_BITRATE_BPS` /
  `BWE_MAX_BITRATE_BPS` in `main.m`). REMB / TWCC estimates outside this
  range are clamped.
- **Initial estimate** is seeded with `VIDEO_BITRATE_KBPS` so the first
  keyframe goes out at the configured target instead of the compile-time
  BWE default.
- **BWE event handler** (`NANORTC_EV_BITRATE_ESTIMATE`) logs the direction
  (`UP`/`DOWN`/`STABLE`) and source (`REMB` or `TWCC`) of each update,
  then recomputes the aggregate minimum across all active viewers and
  drives `vt_encoder_set_bitrate()`. A shared encoder serves every
  viewer, so the slowest link wins.
- **Stats tick** every 2 seconds dumps `fps`, `send_bitrate`,
  `estimated_bitrate`, `fraction_lost`, and `rtt` per session using
  `nanortc_get_track_stats()`.

### Verifying the loop end-to-end

1. Start the app and connect a Chrome viewer on the same LAN. You should
   see `BWE ... via REMB` events or (against recent Chrome builds)
   `BWE ... via TWCC` events within a few seconds of media flowing.
2. Throttle the link (e.g. `pfctl` / `dnctl` on macOS, or browser
   DevTools → Network throttling in the receiving tab) and watch:
   - `BWE DOWN via TWCC` events fire,
   - `[bwe] encoder target → NNN kbps` applies a lower rate,
   - `[stats N] send=... kbps` converges to the new target,
   - `loss=...%` climbs transiently and then recedes.
3. Lift the throttle; expect `BWE UP` events and the encoder returning
   to the upper bound. The `[bwe]` line only prints when the aggregate
   changes by ≥5 % and no more than once per 500 ms to keep the
   hardware encoder from thrashing.

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
