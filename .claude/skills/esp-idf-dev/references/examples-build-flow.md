# Per-example build flow and error recovery

Load this when the user wants to build, flash, or debug one of the
four `examples/esp32_*` projects, or when a specific ESP-IDF build
error shows up.

## Common preamble

Every example follows the same top-level shape:

```
examples/esp32_<which>/
├── CMakeLists.txt              # EXTRA_COMPONENT_DIRS=../.. + IDF project.cmake
├── dependencies.lock           # resolved component versions (in git)
├── main/
│   ├── CMakeLists.txt
│   ├── Kconfig.projbuild       # example-specific menuconfig knobs
│   ├── idf_component.yml       # registry dependencies
│   └── *.c
├── partitions.csv              # custom partition layout
├── sdkconfig.defaults          # base Kconfig overrides
└── sdkconfig.defaults.<target> # optional, merged when TARGET matches
```

`sdkconfig` and `managed_components/` are regenerated on demand and
are not in git.

Always enter the example directory before running `idf.py`:

```bash
cd examples/esp32_<which>
```

## esp32_datachannel — minimal DataChannel echo

**Target:** `esp32`, `esp32s3` (any WiFi-capable chip).
**What it does:** ESP32 hosts a local HTTP page, browser connects,
DataChannel echoes messages back. Zero external signaling.

### Cold-start build

```bash
cd examples/esp32_datachannel
idf.py set-target esp32s3          # or esp32, esp32c3, etc.
idf.py menuconfig
# → "ESP32 DataChannel Example" → set WiFi SSID and Password
idf.py build
```

The WiFi credentials come from `protocol_examples_common` (pulled
from `$IDF_PATH/examples/common_components/...`). That component is
referenced by `path:` in `main/idf_component.yml`, not from the
registry — it's part of the IDF distribution itself.

### Flash and monitor

```bash
idf.py -p /dev/tty.usbmodem* flash monitor    # macOS example
# Ctrl-] exits the monitor
```

Expected serial output (summary):

```
I (xxx) nanortc_dc: WiFi connected
I (xxx) nanortc_dc: Station IP: 192.168.1.xxx
I (xxx) nanortc_dc: HTTP server started on port 80
I (xxx) nanortc_dc: Open http://192.168.1.xxx/ in your browser
... ICE connected → DTLS connected → SCTP connected → DataChannel open
```

## esp32_audio — Opus over SRTP

**Target:** `esp32s3` (for on-board codec support), `esp32p4`.
**What it does:** audio-only WebRTC session with Opus encoded frames.

### Notable dependency

```yaml
# examples/esp32_audio/main/idf_component.yml
dependencies:
  protocol_examples_common:
    path: ${IDF_PATH}/examples/common_components/protocol_examples_common
  espressif/esp_audio_codec:
    version: ">=2.4.0"
```

`esp_audio_codec` handles Opus framing. If you need a different
codec (G.711, AAC) the same component exposes them — check its
README before adding a second codec dependency.

### Cold-start build

Same as datachannel. If you bump audio quality:

- `NANORTC_JITTER_SLOTS` from 16 → 32 for high-jitter paths.
- `NANORTC_JITTER_SLOT_DATA_SIZE` from 160 → 320 for Opus @128kbps.

Both are in the root `Kconfig` under NanoRTC Configuration.

## esp32_video — H.264 video playback

**Target:** `esp32s3` typically (needs PSRAM for NAL reassembly).
**What it does:** streams embedded H.264 video blobs over RTP/SRTP.

### Embedded video blobs

The video payload is compiled into flash via `target_add_binary_data`
or `COMPONENT_EMBED_FILES` in `main/CMakeLists.txt`. Read that file
before tweaking — the blob path is relative to `main/`.

### Cold-start build

Same as audio. Memory tuning:

- `NANORTC_VIDEO_NAL_BUF_SIZE` from 8192 → 32768 for 720p, 65536
  for 1080p keyframes.
- `NANORTC_OUT_QUEUE_SIZE` from 16 → 32 for bursty keyframe FU-A.
- `NANORTC_MEDIA_BUF_SIZE` from 1232 → 1500 for jumbo-MTU paths.

## esp32_camera — ESP32-P4 camera streaming

**Target:** `esp32p4` exclusively — the example depends on
`esp_video` and the MIPI-CSI pipeline.
**What it does:** captures from an onboard camera sensor, H.264
encodes (hardware), pushes over WebRTC.

This is the most complex example. Its dependency list:

```yaml
dependencies:
  protocol_examples_common: { path: ${IDF_PATH}/... }
  espressif/esp_wifi_remote: '*'     # ESP32-C6 co-processor for WiFi
  espressif/esp_hosted: '*'
  espressif/esp_capture: ^0.8.1
  espressif/esp_audio_codec: '~2.4'
  espressif/esp_board_manager: ^0.5.7
```

### esp_board_manager cold start — special handling

On first `idf.py set-target esp32p4` with an empty
`managed_components/`, the example's top-level `CMakeLists.txt`
pre-populates the component manager cache and invokes
`gen_bmgr_config_codes.py` to generate
`components/gen_bmgr_codes/board_manager.defaults`. Without that
generated file the build fails deep inside a `#include
"dev_audio_codec.h"`.

**The CMakeLists handles this automatically when there's exactly
one board directory under `boards/`.** If there are multiple or
none, it aborts with an explicit error:

```
components/gen_bmgr_codes/board_manager.defaults is missing and
3 board directories were found: esp32_p4_nano, m5stack_p4, ...
Pick one explicitly and re-run:

    python managed_components/espressif__esp_board_manager/\
        gen_bmgr_config_codes.py -b <board-name> -c boards
```

In that case: run the printed command, then re-run `idf.py build`.

### Cold-start build

```bash
cd examples/esp32_camera
idf.py set-target esp32p4            # triggers the board manager
                                     # auto-generation path
idf.py build                         # or `idf.py reconfigure` first
                                     # if you want to stage it
```

If you deleted `managed_components/` for a clean start, the
CMakeLists.txt falls back to driving
`idf_component_manager.core.ComponentManager.prepare_dep_dirs()`
directly before calling the generator — that code path is already
there; don't re-implement it.

### Flash

```bash
idf.py -p /dev/tty.usbmodem* flash monitor
```

Connect over the P4 ethernet-over-USB-JTAG port (the one that
enumerates as a composite device).

### Multi-board troubleshooting

If you want a second board:

```bash
cp -r boards/esp32_p4_nano boards/my_new_board
# edit boards/my_new_board/*.yml for your hardware
rm components/gen_bmgr_codes/board_manager.defaults
# Either delete all but one board dir, or pass -b explicitly:
python managed_components/espressif__esp_board_manager/\
    gen_bmgr_config_codes.py -b my_new_board -c boards
idf.py build
```

## Common `idf.py` commands

Run these from any example directory:

| Command | Purpose |
|---|---|
| `idf.py set-target <target>` | One-time per workspace — regenerates build dir |
| `idf.py menuconfig` | Interactive Kconfig editor |
| `idf.py reconfigure` | Re-run CMake without full rebuild (useful after editing `idf_component.yml`) |
| `idf.py build` | Incremental compile |
| `idf.py fullclean` | Delete `build/` — use when changing target or toolchain |
| `idf.py -p <port> flash monitor` | Flash + serial console |
| `idf.py -p <port> erase-flash` | Wipe the entire flash (including NVS) |
| `idf.py size` / `size-components` / `size-files` | Size breakdown of the built binary |
| `idf.py partition-table` | Render `partitions.csv` in CSV form |
| `idf.py app-flash` | Flash only the app partition (faster than full flash) |

## Error recovery

### `Failed to resolve component <name>`

The component manager couldn't find or download the component.

1. Check network connectivity (the registry is
   `components.espressif.com`).
2. Verify the namespace/name spelling in `main/idf_component.yml`.
3. For components specified by `path:`, verify the path exists
   (especially the `${IDF_PATH}/...` ones — `IDF_PATH` must be
   exported).
4. Delete `dependencies.lock` and re-run `idf.py reconfigure` to
   force a fresh resolution.

### `No serial port found` or `Could not open port`

1. Physical connection — unplug / re-plug the USB cable.
2. Port discovery — `ls /dev/tty.usbmodem* /dev/cu.usbserial-*`
   (macOS) or `ls /dev/ttyUSB* /dev/ttyACM*` (Linux).
3. Permissions (Linux) — `sudo usermod -aG dialout $USER` once,
   then re-login. Do **not** sudo `idf.py` as a shortcut; that
   corrupts the build dir.
4. Already in use — another `idf.py monitor` or screen session may
   hold the port. `lsof <port>` will show the holder.
5. On macOS, driver install required for some USB-serial chips
   (CP210x, CH340). The ESP32 DevKits use the onboard USB-JTAG
   which needs no driver.

### `flash size doesn't match`

`sdkconfig.defaults` specifies a flash size (e.g. 4 MB) that
differs from what the chip has (e.g. 8 MB). Either:

- Update `CONFIG_ESPTOOLPY_FLASHSIZE_*` in
  `sdkconfig.defaults.<target>`, or
- `idf.py set-target <target>` then `idf.py menuconfig` → Serial
  flasher → Flash size, save, build.

### `region 'iram0_0_seg' overflowed`

Built binary is too large for IRAM. Common fixes for this repo:

- `NANORTC_FEATURE_*` — disable features not used (TURN if LAN-only,
  H.265 if only H.264 is needed).
- `CONFIG_COMPILER_OPTIMIZATION_SIZE=y` (−Os) instead of −Og.
- `CONFIG_NANORTC_LOG_LEVEL=0` (ERROR only) in production builds.

See `docs/engineering/memory-profiles.md` for the full trim list.

### `component 'driver' not found` (v6.0+)

v6.0 split the driver component. `REQUIRES driver` no longer pulls
everything — add the specific sub-component:

```cmake
# main/CMakeLists.txt
idf_component_register(
    SRCS ...
    REQUIRES esp_driver_gpio esp_driver_uart  # instead of just 'driver'
)
```

Reference the v6.0 migration guide:
`https://docs.espressif.com/projects/esp-idf/en/v6.0/<target>/migration-guides/release-6.x/peripherals.html`

### `board_manager.defaults` errors on esp32_camera

Already covered above. Summary:

- Exactly one board under `boards/` → auto-generated, no action.
- Zero boards → copy `boards/esp32_p4_nano` as a template.
- Multiple boards → delete extras or run `gen_bmgr_config_codes.py`
  manually with `-b <board-name>`.
