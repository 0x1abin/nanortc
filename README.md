# NanoRTC

A Sans I/O, pure C WebRTC implementation for RTOS and embedded systems.

> **AI-native implementation**: Every line of code in this repository — library source, tests, build system, CI, documentation, and examples — is written by AI coding agents. Humans steer architecture and verify correctness; agents execute. See [How this project is built](#how-this-project-is-built) for details.

## What is NanoRTC?

NanoRTC is a WebRTC protocol stack designed from the ground up for resource-constrained microcontrollers running FreeRTOS, Zephyr, RT-Thread, and other RTOS platforms.

**Sans I/O architecture** — Inspired by [str0m](https://github.com/algesten/str0m) (Rust), NanoRTC is a pure state machine. It never touches sockets, threads, memory allocation, or clocks. Your application owns the event loop and all I/O. This makes NanoRTC portable to any platform and testable without a network.

```
                     ┌─────────────────────────┐
  UDP bytes ────────►│                         │──────► bytes to send
  monotonic time ───►│  nanortc_t              │──────► application events
  user commands ────►│  (pure state machine)   │──────► next timeout (ms)
                     │                         │
                     │  No sockets. No threads.│
                     │  No malloc. No clocks.  │
                     └─────────────────────────┘
```

## Features

- **Orthogonal feature flags** — Include only what you need:

| Configuration | Flash (.text) | RAM (sizeof) | Flags |
|--------------|---------------|-------------|-------|
| Core only | 16.5 KB | 10.0 KB | DC=OFF AUDIO=OFF VIDEO=OFF |
| DataChannel | 27.9 KB | 19.7 KB | DC=ON |
| Audio only | 25.1 KB | 21.5 KB | DC=OFF AUDIO=ON |
| DataChannel + Audio | 36.4 KB | 31.2 KB | DC=ON AUDIO=ON |
| Media only (no DC) | 27.4 KB | 42.7 KB | DC=OFF AUDIO=ON VIDEO=ON |
| Full media | 38.7 KB | 52.4 KB | DC=ON AUDIO=ON VIDEO=ON |

> Measured on ESP32-P4 (RISC-V HP), mbedTLS, -O2.
> `sizeof(nanortc_t)` is the full per-connection RAM — no heap allocation.
> Sizes reflect optimized defaults (v0.1). Tune further via [`NANORTC_CONFIG_FILE`](docs/engineering/memory-profiles.md).

Any combination works — audio without DataChannel, video without audio, etc.

- **ICE** — Controlled (answerer) and controlling (offerer) roles, trickle ICE, ICE restart
- **DTLS 1.2** — Via pluggable crypto provider (mbedtls or OpenSSL)
- **SCTP** — Minimal subset for WebRTC DataChannels (reliable + unreliable)
- **DataChannel** — DCEP protocol, reliable and unreliable modes
- **RTP/RTCP/SRTP** — Audio and video media transport, H.264 FU-A packetization
- **SDP** — Offer/answer negotiation, multi-track media
- **NAT traversal** — STUN server-reflexive discovery + TURN relay client (optional, `NANORTC_FEATURE_TURN`)
- **Bandwidth estimation** — REMB-based receiver BWE for adaptive video
- **Single external dependency** — Only mbedtls (built-in on ESP-IDF, Zephyr, RT-Thread, STM32)

## Quick Start

```bash
# Build (Linux/macOS) — default: DataChannel only
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure

# Enable audio + video
cmake -B build -DNANORTC_FEATURE_AUDIO=ON -DNANORTC_FEATURE_VIDEO=ON

# With OpenSSL (for Linux host development)
cmake -B build -DNANORTC_CRYPTO=openssl

# Build examples (full media)
cmake -B build -DNANORTC_FEATURE_DATACHANNEL=ON -DNANORTC_FEATURE_AUDIO=ON \
      -DNANORTC_FEATURE_VIDEO=ON -DNANORTC_CRYPTO=openssl -DNANORTC_BUILD_EXAMPLES=ON
cmake --build build

# ESP-IDF
idf.py build
```

## Usage

### Answerer (Controlled)

```c
#include "nanortc.h"

nanortc_t rtc;
nanortc_config_t cfg = {0};
cfg.crypto = nanortc_crypto_mbedtls();  // or nanortc_crypto_openssl()
cfg.role   = NANORTC_ROLE_CONTROLLED;
nanortc_init(&rtc, &cfg);

nanortc_add_local_candidate(&rtc, "192.168.1.100", 9999);

char answer[4096];
nanortc_accept_offer(&rtc, remote_offer, answer, sizeof(answer), NULL);
// send answer back via signaling
```

### Offerer (Controlling)

```c
nanortc_config_t cfg = {0};
cfg.crypto = nanortc_crypto_mbedtls();
cfg.role   = NANORTC_ROLE_CONTROLLING;
nanortc_init(&rtc, &cfg);

nanortc_add_local_candidate(&rtc, "192.168.1.200", 9999);
nanortc_create_datachannel(&rtc, "chat", NULL);  // register DataChannel before offer

char offer[4096];
nanortc_create_offer(&rtc, offer, sizeof(offer), NULL);
// send offer, receive answer via signaling
nanortc_accept_answer(&rtc, remote_answer);
```

### Event Loop

Your application drives the event loop — NanoRTC never touches sockets or clocks:

```c
for (;;) {
    nanortc_output_t out;
    while (nanortc_poll_output(&rtc, &out) == NANORTC_OK) {
        switch (out.type) {
        case NANORTC_OUTPUT_TRANSMIT:
            sendto(fd, out.transmit.data, out.transmit.len, ...);
            break;
        case NANORTC_OUTPUT_EVENT:
            if (out.event.type == NANORTC_EV_DATACHANNEL_DATA && !out.event.datachannel_data.binary) {
                nanortc_datachannel_send_string(&rtc, out.event.datachannel_data.id,
                                               (const char *)out.event.datachannel_data.data);
            } else if (out.event.type == NANORTC_EV_DISCONNECTED) {
                goto done;
            }
            break;
        case NANORTC_OUTPUT_TIMEOUT:
            break;  // set select()/poll() timeout to out.timeout_ms
        }
    }

    // Wait for network data or timeout, then:
    nanortc_handle_input(&rtc, &(nanortc_input_t){
        .now_ms = now_ms, .data = buf, .len = len, .src = src });   // got data
    nanortc_handle_input(&rtc, &(nanortc_input_t){ .now_ms = now_ms }); // timeout only
}
done:
nanortc_disconnect(&rtc);
nanortc_destroy(&rtc);
```

## Platform Support

| Platform | Status | Notes |
|----------|--------|-------|
| Linux / macOS | Host development & testing | OpenSSL or mbedtls |
| ESP-IDF (ESP32) | Primary embedded target | Built-in mbedtls, lwIP |
| Zephyr | Supported | Built-in mbedtls, lwIP |
| RT-Thread | Supported | mbedtls package, lwIP |
| STM32 + FreeRTOS | Supported | ST-distributed mbedtls, lwIP |
| NuttX | Supported | POSIX-compatible sockets |

## Project Structure

```
include/nanortc.h          Single public API header
src/                        Protocol modules (Sans I/O, no platform deps)
crypto/                     Pluggable crypto providers (mbedtls, openssl)
tests/                      Unit tests + end-to-end tests (no network needed)
tests/interop/              Interop tests against libdatachannel (C++)
examples/                   Application templates
  common/                   Reusable event loop, signaling, media source
  browser_interop/          Browser-based interop test harness
  macos_camera/             macOS camera/mic → browser streaming (multi-viewer)
  sample_data/              Media samples (git submodule)
docs/                       Design docs, execution plans, engineering standards
```

See [ARCHITECTURE.md](ARCHITECTURE.md) for the module dependency graph and data flow.

## How This Project Is Built

NanoRTC is an experiment in **AI-native software engineering**, inspired by [Harness Engineering](https://openai.com/index/harness-engineering/). The entire codebase is generated by AI coding agents, following the principle: **humans steer, agents execute**.

What this means in practice:

- **Architecture & design** — Human decisions, captured in `docs/design-docs/`
- **All code** — Written by AI agents: library source, tests, CI, build system, documentation
- **Quality gates** — Mechanically enforced via CI: forbidden includes, no malloc, symbol naming, format checks, 6-combo feature flag build matrix, AddressSanitizer
- **RFC compliance** — Protocol implementations follow RFCs as the authoritative standard, not reference code
- **Continuous verification** — `./scripts/ci-check.sh` runs the same checks locally that run in GitHub Actions. Auto-detects `ccache`, keeps build dirs across runs for incremental compilation, and exposes `--fast` for tight pre-push loops (skips low-yield combos and the libdatachannel interop suite — seconds rather than minutes)

The repository structure itself is designed for agent legibility: [AGENTS.md](AGENTS.md) serves as the entry point, with progressive disclosure into deeper documentation. Constraints are enforced by code, not by convention.

## Documentation

| Document | Description |
|----------|------------|
| [AGENTS.md](AGENTS.md) | Agent entry point — build commands, mandatory rules |
| [Build Guide](docs/guide-docs/build.md) | Build commands, feature flags, fuzz, coverage, ESP-IDF |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Module dependency graph, layer model, data flow |
| [Design Specification](docs/design-docs/nanortc-design-draft.md) | Full authoritative design reference |
| [Core Beliefs](docs/design-docs/core-beliefs.md) | Non-negotiable design principles |
| [Execution Plans](docs/PLANS.md) | Active and completed implementation plans |
| [Quality Score](docs/QUALITY_SCORE.md) | Per-module quality grades |
| [Memory Profiles](docs/engineering/memory-profiles.md) | RAM usage by configuration, tuning guide |
| [Coding Standards](docs/engineering/coding-standards.md) | Naming, style, RFC testing requirements |
| [Safe C Guidelines](docs/engineering/safe-c-guidelines.md) | Banned functions, buffer safety rules |
| [RFC Index](docs/references/rfc-index.md) | Protocol specification references |

## Contributing

NanoRTC is in active development. Phases 1-5 complete (DataChannel, Audio, Video, Quality, Network Traversal). All 18 modules at A grade — fuzz-tested, browser-verified, interop-verified, 80%+ coverage. Resource optimization pass reduces full-media RAM by 34%.

Contributions welcome. Please read [AGENTS.md](AGENTS.md) for build instructions and mandatory rules before submitting changes.

## License

[MIT](LICENSE)
