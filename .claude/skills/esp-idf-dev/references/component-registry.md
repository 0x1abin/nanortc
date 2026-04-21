# ESP Component Registry — search and adopt, don't reinvent

Load this when you're about to write (or help the user write) any
peripheral driver, codec wrapper, display integration, audio
processing pipeline, or networking helper inside
`examples/esp32_*/main/`. The registry has almost certainly already
solved it.

The core discipline is simple: **search first, adopt if it fits,
hand-write only if nothing does**. nanortc itself is the one place
we carry hand-written RFC code — that's earned because there's no
alternative. It's not earned for an LED strip driver.

## Where to search

**Primary — web UI** (has descriptions, target filters, README,
release history):

```
https://components.espressif.com/components?q=<keyword>
```

Use WebFetch on this URL if you want to pull the search results
inline. The registry ships the component list as server-rendered
HTML, with links to each component's detail page at:

```
https://components.espressif.com/components/<namespace>/<name>
```

**Secondary — `idf.py`** (works once the environment is loaded;
output is terse, good for exact-name confirmation):

```bash
idf.py -C examples/esp32_<which> search <term>
```

## Adding a dependency

From inside the example directory:

```bash
cd examples/esp32_<which>
idf.py add-dependency "espressif/<name>^<major>.<minor>"
idf.py reconfigure
```

The `^MAJOR.MINOR` range is the pragmatic default — it accepts
compatible updates within the major version. Use `~MAJOR.MINOR`
(only patch updates) when the component is in early development
and churning.

This updates:

- `main/idf_component.yml` — the version range you requested
- `dependencies.lock` — the resolved exact version

Both files belong in git. The downloaded source under
`managed_components/` does **not** (`.gitignore` already excludes
it; `idf.py reconfigure` re-fetches it).

## Maturity checklist — five questions, two minutes

Before committing to a component, check:

1. **Publisher.** Is it under `espressif/*`? If not, who maintains it?
   Community components from major vendors (Lilygo, Seeed, M5Stack) are
   generally fine. Random individuals with one component need extra
   scrutiny — read the source.

2. **Target coverage.** The component's `idf_component.yml` has a
   `targets:` list (or implicitly supports all). Your build target must
   be in it. ESP32-P4 support is especially uneven for older components
   (it's a 2024 chip).

3. **ESP-IDF version range.** The component's
   `dependencies.idf` must include `<IDF_VER>` (see `version-and-docs.md`
   for how you pinned that). A component pinned to `idf: ^5.2` will
   refuse to install on v6.0.

4. **Release recency.** Check the release history on the registry
   page. No release in 18+ months with open unresponded issues is a
   yellow flag. For anything network-facing (MQTT, HTTP, BLE), recent
   activity matters a lot more than for a GPIO helper.

5. **Repository quality.** Click through to the GitHub repo from the
   component page. Skim recent issues and PRs for signs the component
   is actively maintained. A component registry entry for a project
   with 200 stale issues is not mature — it's advertising.

## nanortc-relevant components already in use

These appear in the repo's `dependencies.lock` files — no need to
re-evaluate; they've been vetted:

| Component | Example using it | What it does |
|---|---|---|
| `protocol_examples_common` (from `IDF_PATH`) | datachannel, audio, video, camera | WiFi STA provisioning via menuconfig, IP event helpers |
| `espressif/esp_h264` | camera | Hardware-accelerated H.264 encoder on ESP32-P4 |
| `espressif/esp_video` | camera | V4L2-style video pipeline (MIPI-CSI, DVP camera capture) |
| `espressif/esp_audio_codec` | audio, camera | Audio encoders/decoders (Opus, AAC, G.711, …) |
| `espressif/esp_capture` | camera | Camera capture abstraction that feeds esp_video |
| `espressif/esp_board_manager` | camera | Board-level peripheral wiring via YAML declarations |
| `espressif/esp_wifi_remote` | camera | WiFi co-processor support (ESP32-P4 borrows WiFi from a linked ESP32-C6) |
| `espressif/esp_hosted` | camera | Host-driver for `esp_wifi_remote` |

## Likely-useful components for new examples

When you're helping the user add a feature to an existing or new
example, these are the components to reach for first before writing
anything:

### Input / control

- `espressif/iot_button` — debounced push button with single / double
  / long press callbacks. Use this any time the user mentions
  buttons, wakeup from sleep via button, or mode switches.
- `espressif/button` — older, simpler alternative. `iot_button` is
  the active one.
- `espressif/knob` — rotary encoder with direction + step events.

### Visual output

- `espressif/led_strip` — WS2812 / SK6812 / APA102 addressable LEDs
  via RMT or SPI. Canonical component; don't hand-write RMT
  sequences.
- `espressif/led_indicator` — higher-level blink/breathe/fade
  patterns on top of plain GPIO or led_strip.
- `espressif/esp_lvgl_port` — LVGL integration glue for common
  display panels. If the user mentions a screen, start here.

### Sensors

- `espressif/bme280`, `espressif/bmp280`, `espressif/ssd1306`, …
  most common parts already have a packaged driver. Search before
  writing an I2C transaction by hand.

### Networking / protocols

- `espressif/mdns` — service discovery. Use instead of the legacy
  `components/mdns` (which was pulled out of IDF in v5.1).
- `espressif/mqtt` / `espressif/mosquitto` — MQTT client /
  broker respectively. ESP-MQTT is already in IDF; use the
  component only when you need newer features.
- `espressif/esp_websocket_client` — WebSocket client.
- `espressif/esp_tinyusb` — TinyUSB wrapper (CDC, MSC, HID, video).

### Media

- `espressif/esp_h264`, `espressif/esp_h265` — video codecs.
- `espressif/esp_audio_codec` — audio codecs.
- `espressif/gmf_audio` (GMF = Generic Media Framework) — pipeline
  DSP for audio on top of esp_audio_codec.

## When hand-writing is justified

Only in these cases:

1. **RFC protocol layers belonging to nanortc itself.** STUN, DTLS,
   SCTP, RTP, SRTP, ICE — this is the library's core competency.
   Never pull in a third-party WebRTC component to replace nanortc
   code; that defeats the purpose of the library.

2. **Glue between a registry component and nanortc's input/output
   API.** The registry component does the hardware work; the glue
   marshalls its events into `nanortc_handle_input()` or drives
   its output from `nanortc_poll_output()`. The glue is yours to
   write — it's project-specific.

3. **Custom hardware** where no component exists (e.g. a
   one-of-a-kind breakout board). First check
   `boards/` conventions in `espressif/esp_board_manager` — you may
   be able to describe the hardware in YAML and avoid C code.

## Anti-patterns

- **Do not `git submodule` a component.** The component manager is
  the sanctioned mechanism. Submodules don't track IDF version
  compatibility and break on `idf.py reconfigure`.
- **Do not copy component source into the example tree.** That
  fork now owns security updates and IDF-version bumps. Use
  `idf.py add-dependency` and let the manager resolve versions.
- **Do not bypass the component manager with
  `EXTRA_COMPONENT_DIRS` for third-party code.** We use
  `EXTRA_COMPONENT_DIRS` for nanortc itself (parent repo) — that's
  its one legitimate use in these examples. Registry components go
  through the manifest.
- **Do not pick a component just because it has a matching name.**
  Five components might match your search; only one of them might
  be maintained and target-compatible. Run the checklist.
