# Version detection and documentation lookup

Load this when you need to (a) pin the installed ESP-IDF version for
the session, (b) look up an API / Kconfig / migration guide for the
exact installed version, or (c) judge whether something the user
mentioned still exists in that version.

## Detecting the installed version

Three ways, in order of preference:

1. **Preferred — `idf.py` is on PATH** (i.e. `get_idf` was run):

   ```bash
   idf.py --version
   # ESP-IDF v5.5.4
   ```

2. **If `idf.py` isn't available but `IDF_PATH` is set:**

   ```bash
   cat "$IDF_PATH/version.txt"
   # v5.5.4
   ```

3. **Last resort — parse the CMake version file:**

   ```bash
   grep -E 'IDF_VERSION_(MAJOR|MINOR|PATCH)' \
       "$IDF_PATH/tools/cmake/version.cmake"
   ```

Derive two strings for the rest of the session:

- `<IDF_VER>` = full version, e.g. `v5.5.4` — mention once for context
- `<IDF_MAJOR>` = `5` — use when gating advice by major release

(The patch version matters when looking up changelog entries; major
matters when answering "is this API still here".)

## Documentation lookup — local first

**`$IDF_PATH/docs/` ships the source documentation for the exact
installed version.** The RST files are structured text, very friendly
to Read/Grep, and guaranteed to match the compiled toolchain — no
version-URL dance needed. Prefer this over any online source.

### Layout

```
$IDF_PATH/docs/
├── en/                               ← English, canonical
│   ├── api-reference/
│   │   ├── peripherals/gpio.rst
│   │   ├── peripherals/i2c.rst
│   │   ├── network/esp_wifi.rst
│   │   ├── system/freertos.rst
│   │   ├── storage/nvs_flash.rst
│   │   └── kconfig.rst               ← all Kconfig option names
│   ├── api-guides/
│   │   ├── build-system.rst
│   │   ├── linker-script-generation.rst
│   │   └── jtag-debugging/
│   ├── migration-guides/
│   │   ├── release-5.x/
│   │   └── release-6.x/              ← present on v6.0 checkouts
│   ├── get-started/
│   ├── security/
│   └── hw-reference/
└── zh_CN/                            ← Chinese translation, same tree
```

### Recipes

Look up a specific API signature — prefer the **header file** (it
has the authoritative Doxygen comments):

```bash
# GPIO API in v5.x
cat "$IDF_PATH/components/esp_driver_gpio/include/driver/gpio.h"
# v4.x / legacy path
cat "$IDF_PATH/components/driver/include/driver/gpio.h"
```

Find which header a function is in:

```bash
# Find declaration of esp_wifi_init
grep -rn "esp_wifi_init(" "$IDF_PATH/components/esp_wifi/include/"
```

Look up a Kconfig option:

```bash
# All options named CONFIG_*SPIRAM*
grep -rn "config.*SPIRAM" "$IDF_PATH/components/esp_psram/Kconfig"
# Or browse the merged list
less "$IDF_PATH/docs/en/api-reference/kconfig.rst"
```

Read the prose doc for a subsystem:

```bash
less "$IDF_PATH/docs/en/api-reference/peripherals/gpio.rst"
```

Check the migration guide (v5→v6, v4→v5, etc.):

```bash
ls  "$IDF_PATH/docs/en/migration-guides/"
less "$IDF_PATH/docs/en/migration-guides/release-6.x/peripherals.rst"
```

Look for example code using an API:

```bash
grep -rln "gpio_install_isr_service" "$IDF_PATH/examples/" | head -5
```

Component-specific READMEs often have runnable snippets:

```bash
ls "$IDF_PATH/components/esp_wifi/README.md" 2>/dev/null
```

### Chinese docs

`$IDF_PATH/docs/zh_CN/` mirrors the `en/` tree. Use it when the user
is asking in Chinese and a translated explanation would land better.
Technical content is the same — the structure is identical to `en/`.

## Online docs — when to reach for them

Read `$IDF_PATH/docs/` by default. Fall back to
`docs.espressif.com` only when:

- **The user's installed version differs from what they're
  deploying on.** E.g. they have v5.5 locally but need to know if an
  API exists on v4.4 LTS for a field deployment.
- **Migration guide for a version not present in the checkout.** The
  v6.x migration guide only appears after a v6.x checkout exists.
- **Cross-version comparison** ("is this still the same in v6.0?").

URL template:

```
https://docs.espressif.com/projects/esp-idf/en/v<MAJOR>.<MINOR>/<TARGET>/
```

`<TARGET>` is hyphenated (`esp32-s3`, `esp32-c3`, `esp32-p4`, …). The
patch version doesn't appear — Espressif hosts one doc tree per
minor release line.

For the current user's v5.5.x install:

```
https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32-p4/
```

## Breakage points across versions

When the user is crossing a version boundary, check the migration
guide first (`$IDF_PATH/docs/en/migration-guides/`). The big-ticket
items to remember:

### v4.4 LTS → v5.x

- **I2C driver**: legacy `driver/i2c.h` is deprecated; new
  `driver/i2c_master.h` / `driver/i2c_slave.h` landed in v5.2.
- **ADC**: `driver/adc.h` → `esp_adc/adc_oneshot.h` +
  `esp_adc/adc_continuous.h` in v5.0.
- **GPTimer**: `driver/timer.h` → `driver/gptimer.h` in v5.0.
- **TinyUSB** moved to a managed component.
- **Event loop**: only `esp_event_loop_create_default` remains.

### v5.x → v6.0

- **`esp_driver_*` namespace split**: each peripheral is its own
  component (`esp_driver_gpio`, `esp_driver_uart`, `esp_driver_i2c`,
  …). `REQUIRES driver` no longer pulls everything.
- **Legacy I2C driver (`driver/i2c.h`) removed.** Must migrate to
  `driver/i2c_master.h`.
- **CMake minimum bumped to 3.22.**
- **FreeRTOS V10.5 prefix unification**; header paths cleaned up.
- **`MINIMAL_BUILD` Kconfig** added for slim bring-up builds.

### Within v5.x

- **v5.0 → v5.1**: power-management API flattened
  (`esp_pm_config_*_t` → `esp_pm_config_t`).
- **v5.1 → v5.2**: new I2C driver GA; BLE stack default swap on
  some targets.
- **v5.2 → v5.3**: ESP32-C5, ESP32-P4 officially supported.
- **v5.3 → v5.5**: `esp_video` + modern camera pipeline landed
  (used by `examples/esp32_camera/`).

## When to trust memory vs open a file

**Open a file (or grep the tree) when:**

- User asks about a specific function signature, param, enum, or
  return type — read the header directly, it's the source of truth.
- User reports a compile error mentioning an API name — verify in
  `$IDF_PATH/components/<...>/include/` before explaining.
- Kconfig key name or default value matters — grep
  `$IDF_PATH/components/<...>/Kconfig`.
- Any v4 ↔ v5 ↔ v6 crossing question.

**Trust memory when:**

- Conceptual questions ("what is a partition table", "how does
  Wi-Fi STA connect"). Concepts are stable across versions.
- Something already documented in this repo's `docs/` or `Kconfig`.

**Never guess.** A wrong API call costs the user a flash cycle.
Reading a local file costs ~zero.
