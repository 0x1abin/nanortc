---
name: esp-idf-dev
description: >
  Project-local ESP-IDF workflow for nanortc. Loads the user's get_idf
  environment, detects the installed ESP-IDF version, and pins every
  documentation lookup and API reference to that exact version so the
  advice never drifts across v4.x / v5.x / v6.0 breakage points.
  ALWAYS searches ESP Component Registry (components.espressif.com)
  before scaffolding any peripheral driver, codec, networking glue,
  display, or audio processing in examples/esp32_* — the rule is
  "find the mature component first, hand-write code only as the last
  resort, because the nanortc library itself already owns enough
  hand-written RFC code that we don't want to multiply it with
  reinvented driver wheels".
  Use this skill whenever the user works on examples/esp32_datachannel,
  examples/esp32_audio, examples/esp32_video, examples/esp32_camera,
  edits the root Kconfig / idf_component.yml, runs idf.py
  build/flash/monitor, tunes memory profiles, or asks anything about
  ESP-IDF, ESP32 (all variants S2/S3/C2/C3/C5/C6/H2/P4), FreeRTOS,
  menuconfig, sdkconfig, esp_board_manager, partition tables, OTA,
  NVS, WiFi/BLE stacks, or any espressif/* component on the registry.
  Trigger on: "esp-idf", "esp32", "esp32-s3", "esp32-p4", "idf.py",
  "menuconfig", "sdkconfig", "烧录", "刷固件", "构建固件",
  "build firmware", "flash to esp32", "板子", "开发板",
  "esp32 示例", "example", "板载相机", "board_manager",
  "添加组件", "加 esp32 依赖", "找现成组件", "重复造轮子",
  "add-dependency", "components.espressif.com".
---

# esp-idf-dev — NanoRTC ESP-IDF Workflow

This skill captures the ESP-IDF development loop specific to the nanortc
repo: how to load the toolchain, how to stay honest about which
ESP-IDF version's documentation you're quoting, how to build and flash
each of the four `examples/esp32_*` projects, and — most importantly —
the discipline of checking the ESP Component Registry before writing
any new peripheral or codec code.

Why this matters: nanortc is itself a hand-written RFC-compliant
library. It earns that cost because there's no equivalent Sans I/O
WebRTC stack for embedded targets. That dispensation does **not**
extend to button drivers, LED strips, camera sensors, or audio
codecs — those already have maintained `espressif/*` components and
writing them from scratch just creates more code for us to carry.

## Step 0 — Load and pin the environment

Always do this first when the user's request touches ESP-IDF. Do not
skip even if they seem to have a shell ready, because `idf.py`
commands will fail silently or pick up the wrong toolchain without it.

1. Check whether the environment is already exported:

   ```bash
   echo "${IDF_PATH:-NOT_SET}"
   ```

2. If `NOT_SET`, tell the user to run the loader themselves in this
   session — they prefer the `get_idf` alias:

   > Run `! get_idf` in the prompt (the `!` prefix runs it in this
   > session so the exports land in our Bash tool environment).

   Do **not** try to `cd` into an IDF checkout and `source export.sh`
   yourself on a random path — the user's memory is explicit that
   hardcoded `export.sh` paths should not be used.

3. Once `IDF_PATH` is set, capture the version. This one command
   covers the common case; fall back to `references/version-and-docs.md`
   if the tool isn't on PATH yet:

   ```bash
   idf.py --version
   # → ESP-IDF v5.5.4
   ```

4. Remember `<IDF_VER>` (e.g. `v5.5.4`) and the major number for
   the rest of the session — mention the version once when setting
   context, and use the major to gate advice (v4 vs v5 vs v6 differ
   meaningfully). The doc you'll actually be reading lives under
   `$IDF_PATH/` and always matches this version automatically.

## Step 1 — Documentation discipline

When the user asks about any ESP-IDF API, Kconfig option, peripheral,
or subsystem, do not answer from memory. ESP-IDF has shipped breaking
API changes across v4 → v5 → v6; the version the user has installed
dictates which call is still valid.

**The installed IDF checkout IS the documentation.** Prefer reading
local files over anything online — they're guaranteed to match the
compiled toolchain:

| What you want | Where to look |
|---|---|
| Function signature / Doxygen comment | `$IDF_PATH/components/<name>/include/**/<header>.h` |
| Subsystem prose doc (English) | `$IDF_PATH/docs/en/api-reference/**/<subsystem>.rst` |
| Subsystem prose doc (Chinese) | `$IDF_PATH/docs/zh_CN/api-reference/**/<subsystem>.rst` |
| Kconfig option name + help | Grep `$IDF_PATH/components/<name>/Kconfig*` |
| Migration guide | `$IDF_PATH/docs/en/migration-guides/release-<ver>/` |
| Example code using an API | `grep -rln <symbol> $IDF_PATH/examples/` |

Use Read / Grep / Glob against `$IDF_PATH` directly. Don't WebFetch
for things the checkout already has.

**Fall back to `docs.espressif.com` only when:** the user's
installed version differs from the deployment target, the
migration guide is for a version not in the checkout, or you need
to compare across versions. URL template:
`https://docs.espressif.com/projects/esp-idf/en/v<MAJOR>.<MINOR>/<target>/`
(target hyphenated: `esp32-s3`, `esp32-p4`, …).

For the full layout of `$IDF_PATH/docs/`, concrete grep recipes,
and the v4→v5→v6 breakage cheat sheet, read
`references/version-and-docs.md`.

## Step 2 — Component-first rule

Before writing any new code for a peripheral, codec, network helper,
display driver, or audio pipeline stage, search the ESP Component
Registry:

```
https://components.espressif.com/components?q=<keyword>
```

If there's a hit under the `espressif/*` namespace (or another
reputable vendor) and it covers the target chip:

```bash
# From inside the relevant examples/esp32_<which>/ directory
idf.py add-dependency "espressif/<name>^<major>.<minor>"   # semver range
idf.py reconfigure                                          # pull it down
```

Keep the manifest edits in the example's `main/idf_component.yml`,
not in a side-loaded file. The component manager commits version
locks to `dependencies.lock` automatically.

**Evaluate maturity before adopting:**

1. Publisher — prefer `espressif/*`; other namespaces need a quick
   look at their source repo.
2. Target coverage — the component's `targets:` list must include
   the chip we're building for.
3. Last release date — stale (>18 months, no issues closed) is a
   yellow flag.
4. ESP-IDF compatibility — manifest `dependencies.idf` version range
   must include `<IDF_VER>`.

**When hand-writing is justified:**

- RFC protocol layers for nanortc itself (`src/nano_*.c`). That is
  the nanortc core competency — we're not going to pull in another
  WebRTC stack.
- Hardware glue unique to a one-off board where no abstraction
  exists.

For the decision flow, the component whitelist we already use in
this repo, and the anti-patterns to avoid, read
`references/component-registry.md`.

## Step 3 — Build, flash, and monitor

Everything happens from inside a specific example directory. Never
run `idf.py` from the repo root — there's no top-level ESP-IDF
project, just the component.

General flow (per example):

```bash
cd examples/esp32_<which>
idf.py set-target <esp32|esp32s3|esp32p4|...>   # one-time per workspace
idf.py menuconfig                                # optional
idf.py build
```

Port detection before flashing:

```bash
# macOS
ls /dev/tty.usbmodem* /dev/cu.usbserial-* 2>/dev/null

# Linux
ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null
```

Then:

```bash
idf.py -p <port> flash monitor
# Ctrl-] to exit monitor
```

Each example has quirks — WiFi credential config for datachannel,
audio buffer knobs for audio, blob embedding for video, and the
`esp_board_manager` cold-start dance for camera. For the per-example
recipes and the common error recovery catalogue (`Failed to resolve
component`, `No serial port found`, `flash size doesn't match`, …),
read `references/examples-build-flow.md`.

## Step 4 — Kconfig tuning and memory profiles

nanortc exposes ~23 knobs under `menu "NanoRTC Configuration"` in
the repo-root `Kconfig`. Defaults are IoT-grade (small). When the
user hits RAM pressure or needs higher throughput:

1. Read the relevant knob's `help` block in `/Kconfig` — every
   knob has a sized-for-what explanation.
2. Read `docs/engineering/memory-profiles.md` for the IoT →
   Standard → Jumbo trim table and the `sizeof(nanortc_t)` /
   `.text` baselines on ESP32-P4.
3. Change the knob via `idf.py menuconfig` (the change lives in
   `sdkconfig`), then persist it by copying the line into
   `sdkconfig.defaults` (or `sdkconfig.defaults.<target>` if
   target-specific). Do **not** commit the raw `sdkconfig` — it's
   regenerated per target and per workspace.
4. Regression-check the size impact:

   ```bash
   ./scripts/measure-sizes.sh --esp32 esp32p4
   ```

Common tuning scenarios:

- Browser peer fragments `ClientHello`: bump `NANORTC_DTLS_BUF_SIZE`
  from 1536 to 2048.
- Multi-track SDPs from browsers: bump `NANORTC_SDP_BUF_SIZE` from
  1024 to 2048.
- 720p / 1080p H.264 keyframes: bump `NANORTC_VIDEO_NAL_BUF_SIZE`
  from 8192 to 32768 or 65536, and `NANORTC_OUT_QUEUE_SIZE` to 32.
- High-throughput DataChannel: bump
  `NANORTC_SCTP_SEND_BUF_SIZE` and `NANORTC_SCTP_MAX_SEND_QUEUE`.

## Step 5 — Adding new examples or dependencies

### New example project

Start from the minimum working layout — copy `examples/esp32_datachannel/`
(no managed components, just `protocol_examples_common` from
`IDF_PATH`):

```
examples/esp32_<new>/
├── CMakeLists.txt               # sets EXTRA_COMPONENT_DIRS to ../..
├── main/
│   ├── CMakeLists.txt           # idf_component_register(...)
│   ├── Kconfig.projbuild        # example-specific config
│   ├── idf_component.yml        # dependencies (start minimal)
│   └── <source>.c
├── partitions.csv
└── sdkconfig.defaults
```

The example's top-level `CMakeLists.txt` must include:

```cmake
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../..")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_<new>)
```

That single line (`EXTRA_COMPONENT_DIRS`) is how the example pulls
in nanortc as a component. Do **not** try to symlink or git-submodule
nanortc into the example — `EXTRA_COMPONENT_DIRS` is the sanctioned
path.

### New dependency

Always go through the component manager:

```bash
cd examples/esp32_<which>
idf.py add-dependency "espressif/<name>^<ver>"
idf.py reconfigure
```

This edits `main/idf_component.yml` (the version range) and writes
`dependencies.lock` (the resolved version). Both go into git; the
downloaded sources under `managed_components/` stay out of git.

### Changes to the top-level nanortc component

If mbedtls isn't enough and you need to add an IDF component to
nanortc's own requirements:

- Update `idf_component.yml` at the repo root (add under `dependencies:`).
- Update the root `CMakeLists.txt` `REQUIRES mbedtls` line to include
  the new component.

Both files must stay in sync. The CI script (`scripts/ci-check.sh`)
does not build the ESP-IDF path, so errors here surface only when
someone re-flashes an example. Mention this cost to the user before
making changes of this kind.

## Rules

- **Run `idf.py` from the example directory**, never from the repo
  root. The top-level project is host-mode CMake; nanortc is only
  an ESP-IDF component.
- **Never add ESP-IDF headers to `src/` or `crypto/`.** That
  violates nanortc's Sans I/O discipline and is enforced by
  `scripts/ci-check.sh`. Platform code belongs in
  `examples/esp32_*/main/`.
- **Never answer ESP-IDF API questions from memory.** Read the
  header in `$IDF_PATH/components/<name>/include/` or the RST under
  `$IDF_PATH/docs/en/` — both match the installed version
  automatically. API drift is real and version-specific.
- **Search the registry before scaffolding any driver / codec /
  protocol.** The rare exceptions are RFC layers belonging to
  nanortc itself.
- **Never auto-commit.** The user controls `git commit` manually
  (per their standing preference). Propose diffs; let them commit.
- **Use Chinese for explanation, English for code and
  commit messages.** Matches the user's stated language preference.
- **Don't change `sdkconfig` directly.** Persist via
  `sdkconfig.defaults` (or `sdkconfig.defaults.<target>`). Raw
  `sdkconfig` is regenerated and target-specific.
- **When the user hits `board_manager.defaults` errors on
  esp32_camera**, read the full recovery flow in
  `references/examples-build-flow.md`. Do not manually patch the
  generated file.

## Quick reference

| Task | Where to look |
|---|---|
| Version detection + `$IDF_PATH/docs/` layout + v4/v5/v6 breakage | `references/version-and-docs.md` |
| Registry search, maturity evaluation, nanortc whitelist | `references/component-registry.md` |
| Per-example build/flash/monitor, error recovery | `references/examples-build-flow.md` |
| ESP-IDF API signature (authoritative) | `$IDF_PATH/components/<name>/include/**/<header>.h` |
| ESP-IDF prose docs (EN / 中文) | `$IDF_PATH/docs/{en,zh_CN}/api-reference/` |
| ESP-IDF example code | `$IDF_PATH/examples/` |
| nanortc Kconfig knob semantics | `/Kconfig` (repo root) |
| Memory profile baselines and trim table | `docs/engineering/memory-profiles.md` |
| General build system (host + ESP-IDF) | `docs/guide-docs/build.md` |
| Byte-level sizing regression | `scripts/measure-sizes.sh` |
