# ESP32 DataChannel Example

ESP32 WebRTC DataChannel echo demo using nanortc. The ESP32 hosts a web page
and acts as an answerer (CONTROLLED). All DataChannel messages are echoed back
to the browser. Zero external dependencies — no signaling server needed.

## Quick Start

### 1. Configure WiFi

```bash
cd examples/esp32_datachannel
idf.py menuconfig
# → "ESP32 DataChannel Example" → set WiFi SSID and Password
```

### 2. Build and Flash

```bash
idf.py build
idf.py flash monitor
```

### 3. Open in Browser

Navigate to `http://<esp32-ip>/` (the IP is printed on the serial console).
The page auto-connects, type a message and see it echoed back.

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
  │  ═══ WebRTC (STUN/DTLS/SCTP) ═│  Direct UDP on LAN
  │                                │
  │  DataChannel "test" ──────→    │  Echo back
  │  ←──── echo ────────────────   │
```

No STUN server is needed — ICE candidates are bundled in the SDP offer/answer
and resolved directly on the LAN.

## Configuration

All settings are in `Kconfig.projbuild` and accessible via `idf.py menuconfig`:

| Setting | Default | Description |
|---------|---------|-------------|
| `EXAMPLE_WIFI_SSID` | `myssid` | WiFi network name |
| `EXAMPLE_WIFI_PASSWORD` | `mypassword` | WiFi password |
| `EXAMPLE_UDP_PORT` | `9999` | Local UDP port for WebRTC |

nanortc buffer sizes can be tuned in the top-level `NanoRTC Configuration` menu.

## Tested Hardware

| Board | SoC | ESP-IDF | Status |
|-------|-----|---------|--------|
| ESP32-S3-DevKitC-1 (N8R2) | ESP32-S3 | v6.0 | Full E2E verified |

## Expected Serial Output

```
I (xxx) nanortc_dc: WiFi connected
I (xxx) nanortc_dc: Station IP: 192.168.1.xxx
I (xxx) nanortc_dc: HTTP server started on port 80
I (xxx) nanortc_dc: Open http://192.168.1.xxx/ in your browser
I (xxx) nanortc_dc: WebRTC task started
I (xxx) nanortc_dc: Got SDP offer (xxx bytes)
I (xxx) nanortc_dc: SDP answer generated (xxx bytes)
I (xxx) nanortc_dc: ICE connected
I (xxx) nanortc_dc: DTLS connected
I (xxx) nanortc_dc: SCTP connected
I (xxx) nanortc_dc: DataChannel open (stream=0)
I (xxx) nanortc_dc: DC string: hello
```

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| No serial output after flash | ESP32-S3 USB-JTAG console not enabled | Ensure `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` in sdkconfig |
| Stuck after flash, no boot | ESP32-S3 needs manual reset after flash via USB-JTAG | Press the RST button |
| Page loads but Connect fails | Browser and ESP32 on different subnets | Ensure same WiFi network |
| WiFi connection failed | Wrong credentials | Check `CONFIG_EXAMPLE_WIFI_SSID` / `CONFIG_EXAMPLE_WIFI_PASSWORD` |
| Browser refresh doesn't reconnect | nanortc re-init failed | Check serial log for error, try power-cycling ESP32 |
