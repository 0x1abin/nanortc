# ESP32 Audio Example

ESP32 WebRTC audio sender demo using nanortc. The ESP32 generates a sine wave
tone, encodes it (PCMU/PCMA/Opus), and streams it to the browser via WebRTC.
Zero external dependencies — no signaling server needed.

## Quick Start

### 1. Configure

```bash
cd examples/esp32_audio
idf.py menuconfig
# → "ESP32 Audio Example" → set WiFi SSID, Password, and codec choice
```

### 2. Build and Flash

```bash
idf.py build
idf.py flash monitor
```

### 3. Open in Browser

Navigate to `http://<esp32-ip>/` (the IP is printed on the serial console).
The page auto-connects and plays the sine wave tone.

## Codec Selection

Select via `idf.py menuconfig` → "ESP32 Audio Example" → "Audio codec":

| Codec | Sample Rate | Channels | Frame Size |
|-------|-------------|----------|------------|
| G.711 mu-law (default) | 8 kHz | mono | 160 bytes |
| G.711 A-law | 8 kHz | mono | 160 bytes |
| Opus | 48 kHz | stereo | ~variable |

All codecs use `espressif/esp_audio_codec` (unified encoder API). PCMU is the default.

## Architecture

```
Browser                          ESP32
  │                                │
  │  GET /  ──────────────────→    │  Serve index.html
  │  ←──────────────────────────   │
  │                                │
  │  POST /offer (SDP) ──────→    │  nanortc_accept_offer()
  │  ←──── SDP answer ─────────   │
  │                                │
  │  ═══ WebRTC (STUN/DTLS/RTP) ══│  Direct UDP on LAN
  │                                │
  │  ←──── Audio (sine wave) ────  │  20ms frames, sendonly
```

## Configuration

All settings are in `Kconfig.projbuild` and accessible via `idf.py menuconfig`:

| Setting | Default | Description |
|---------|---------|-------------|
| `EXAMPLE_WIFI_SSID` | `myssid` | WiFi network name |
| `EXAMPLE_WIFI_PASSWORD` | `mypassword` | WiFi password |
| `EXAMPLE_UDP_PORT` | `9999` | Local UDP port for WebRTC |
| `EXAMPLE_AUDIO_CODEC` | `PCMU` | Audio codec (PCMU/PCMA/Opus) |
| `EXAMPLE_SINE_FREQ_HZ` | `440` | Sine wave frequency |

## Expected Serial Output

```
I (xxx) nanortc_audio: nanortc ESP32 Audio example — G.711 mu-law 8kHz, 440 Hz sine
I (xxx) nanortc_audio: WiFi connected
I (xxx) nanortc_audio: Station IP: 192.168.1.xxx
I (xxx) nanortc_audio: HTTP server started on port 80
I (xxx) nanortc_audio: Open http://192.168.1.xxx/ in your browser
I (xxx) nanortc_audio: WebRTC task started
I (xxx) nanortc_audio: Got SDP offer (xxx bytes)
I (xxx) nanortc_audio: SDP answer generated (xxx bytes)
I (xxx) nanortc_audio: ICE connected
I (xxx) nanortc_audio: DTLS connected — starting audio
```
