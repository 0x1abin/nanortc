# ESP32 DataChannel Example

ESP32 WebRTC DataChannel echo demo using nanortc. The ESP32 acts as an answerer
(CONTROLLED) and echoes all DataChannel messages back to the offerer.

## Prerequisites

- ESP-IDF v5.0+ (tested with v6.0)
- Python 3 for the signaling server
- A browser or the `browser_interop` Linux binary as offerer

## Quick Start

### 1. Configure WiFi

```bash
cd examples/esp32_datachannel
idf.py menuconfig
# → "ESP32 DataChannel Example" → set WiFi SSID and Password
```

Or edit `sdkconfig.defaults` before first build.

### 2. Build and Flash

```bash
idf.py build
idf.py flash monitor
```

### 3. Start the Signaling Server (on your PC)

```bash
cd examples/browser_interop
python3 signaling_server.py --port 8765
```

The server listens for UDP discovery broadcasts on port 19730 by default.
The ESP32 will auto-discover the server on the same LAN.

### 4. Connect from Browser

Open `http://<your-pc-ip>:8765/` in a browser. The browser acts as offerer,
the ESP32 as answerer. Type a message and see it echoed back.

### 5. Connect from Linux CLI

```bash
# Build browser_interop with OpenSSL
cmake -B build -DNANORTC_CRYPTO=openssl -DNANORTC_BUILD_EXAMPLES=ON \
      -DNANORTC_FEATURE_DATACHANNEL=ON
cmake --build build -j$(nproc)

# Run as offerer
./build/examples/browser_interop/browser_interop --offer -s localhost:8765
```

## Architecture

```
┌─────────────┐   HTTP signaling   ┌──────────────────┐   HTTP signaling   ┌──────────┐
│   Browser   │ ←────────────────→ │ signaling_server │ ←────────────────→ │  ESP32   │
│ (offerer)   │                    │   (Python)       │                    │(answerer)│
└──────┬──────┘                    └────────┬─────────┘                    └────┬─────┘
       │                                    │ UDP discovery                     │
       │                                    │←──────────────────────────────────│
       │                                    │                                   │
       └────────────── WebRTC (STUN/DTLS/SCTP/DC) ──────────────────────────────┘
                            Direct UDP P2P
```

## UDP Auto-Discovery

The ESP32 broadcasts `NANORTC_DISCOVER` on UDP port 19730. The signaling
server replies with its HTTP port. This avoids hardcoding the server IP.

If discovery fails (e.g., broadcast blocked), the ESP32 falls back to the
IP configured in Kconfig (`CONFIG_EXAMPLE_SIGNALING_HOST`).

## Configuration

All settings are in `Kconfig.projbuild` and accessible via `idf.py menuconfig`:

| Setting | Default | Description |
|---------|---------|-------------|
| `EXAMPLE_WIFI_SSID` | `myssid` | WiFi network name |
| `EXAMPLE_WIFI_PASSWORD` | `mypassword` | WiFi password |
| `EXAMPLE_SIGNALING_HOST` | `192.168.1.100` | Fallback signaling server IP |
| `EXAMPLE_SIGNALING_PORT` | `8765` | HTTP port |
| `EXAMPLE_UDP_PORT` | `9999` | Local UDP port for WebRTC |
| `EXAMPLE_DISCOVERY_PORT` | `19730` | UDP discovery port |

nanortc buffer sizes can be tuned in the top-level `NanoRTC Configuration` menu.

## Tested Hardware

| Board | SoC | ESP-IDF | Status |
|-------|-----|---------|--------|
| ESP32-S3-DevKitC-1 (N8R2) | ESP32-S3 | v6.0 | Full E2E verified |

## Expected Serial Output

Successful DataChannel echo session:

```
I (xxx) nanortc_dc: WiFi connected
I (xxx) nanortc_dc: Station IP: 192.168.1.xxx
[discovery] Found server at 192.168.1.xxx:8765
I (xxx) nanortc_dc: nanortc ESP32 DC (answerer, udp=192.168.1.xxx:9999, sig=192.168.1.xxx:8765)
I (xxx) nanortc_dc: Waiting for SDP offer...
I (xxx) nanortc_dc: Got SDP offer (xxx bytes)
I (xxx) nanortc_dc: Generated SDP answer (xxx bytes)
I (xxx) nanortc_dc: Sent SDP answer
I (xxx) nanortc_dc: Entering event loop...
I (xxx) nanortc_dc: ICE connected
I (xxx) nanortc_dc: DTLS connected
I (xxx) nanortc_dc: SCTP connected
I (xxx) nanortc_dc: DataChannel open (stream=0)
I (xxx) nanortc_dc: DC string: hello
```

## Automated Testing

The `tests/esp32/test_esp32_dc.py` script runs a fully automated E2E test:

```bash
# Prerequisites: ESP-IDF environment sourced, ESP32-S3 connected,
# browser_interop binary built (cmake -B build -DNANORTC_CRYPTO=openssl -DNANORTC_BUILD_EXAMPLES=ON)

python3 tests/esp32/test_esp32_dc.py                          # default device
python3 tests/esp32/test_esp32_dc.py --port /dev/ttyUSB0      # custom serial port
python3 tests/esp32/test_esp32_dc.py --skip-build --skip-flash # re-test without rebuild
```

The script builds, flashes, starts the signaling server with UDP discovery,
launches the Linux offerer, and verifies the DataChannel echo round-trip.

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| No serial output after flash | ESP32-S3 USB-JTAG console not enabled | Ensure `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` in sdkconfig (set in `sdkconfig.defaults`) |
| Stuck after flash, no boot | ESP32-S3 needs manual reset after flash via USB-JTAG | Press the RST button on the board after flashing |
| Discovery timeout | Broadcast blocked by network/firewall | Configure fallback IP via `idf.py menuconfig` → `EXAMPLE_SIGNALING_HOST` |
| `Join failed` / signaling error | Stale peers in signaling server | Restart `signaling_server.py`, or `curl -X POST <host>:8765/leave?id=0` |
| WiFi connection failed | Wrong credentials | Check `CONFIG_EXAMPLE_WIFI_SSID` / `CONFIG_EXAMPLE_WIFI_PASSWORD` in menuconfig |
