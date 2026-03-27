# NanoRTC

A Sans I/O, pure C WebRTC implementation for RTOS and embedded systems.

> **AI-native implementation**: Every line of code in this repository — library source, tests, build system, CI, documentation, and examples — is written by AI coding agents. Humans steer architecture and verify correctness; agents execute. See [How this project is built](#how-this-project-is-built) for details.

## What is NanoRTC?

NanoRTC is a WebRTC protocol stack designed from the ground up for resource-constrained microcontrollers running FreeRTOS, Zephyr, RT-Thread, and other RTOS platforms.

**Sans I/O architecture** — Inspired by [str0m](https://github.com/algesten/str0m) (Rust), NanoRTC is a pure state machine. It never touches sockets, threads, memory allocation, or clocks. Your application owns the event loop and all I/O. This makes NanoRTC portable to any platform and testable without a network.

```
                     ┌─────────────────────────┐
  UDP bytes ────────►│                         │──────► bytes to send
  monotonic time ───►│  nano_rtc_t             │──────► application events
  user commands ────►│  (pure state machine)   │──────► next timeout (ms)
                     │                         │
                     │  No sockets. No threads.│
                     │  No malloc. No clocks.  │
                     └─────────────────────────┘
```

## Features

- **Orthogonal feature flags** — Include only what you need:

| Flag | What it adds | Flash | RAM (1 conn) |
|------|-------------|-------|-------------|
| `NANO_FEATURE_DATACHANNEL` | SCTP + DCEP DataChannels | ~80 KB | ~60 KB |
| `+ NANO_FEATURE_AUDIO` | RTP/SRTP, jitter buffer | ~130 KB | ~100 KB |
| `+ NANO_FEATURE_VIDEO` | H.264/VP8, bandwidth estimation | ~180 KB | ~160 KB |

Any combination works — audio without DataChannel, video without audio, etc.

- **ICE** — Controlled (answerer) and controlling (offerer) roles
- **DTLS 1.2** — Via pluggable crypto provider (mbedtls or OpenSSL)
- **SCTP** — Minimal subset for WebRTC DataChannels
- **DataChannel** — DCEP protocol, reliable and unreliable modes
- **RTP/RTCP/SRTP** — Audio and video media transport
- **SDP** — Offer/answer negotiation
- **Single external dependency** — Only mbedtls (built-in on ESP-IDF, Zephyr, RT-Thread, STM32)

## Quick Start

```bash
# Build (Linux/macOS) — default: DataChannel only
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure

# Enable audio + video
cmake -B build -DNANO_FEATURE_AUDIO=ON -DNANO_FEATURE_VIDEO=ON

# With OpenSSL (for Linux host development)
cmake -B build -DNANORTC_CRYPTO=openssl

# Build examples (full media)
cmake -B build -DNANO_FEATURE_DATACHANNEL=ON -DNANO_FEATURE_AUDIO=ON \
      -DNANO_FEATURE_VIDEO=ON -DNANORTC_CRYPTO=openssl -DNANORTC_BUILD_EXAMPLES=ON
cmake --build build

# ESP-IDF
idf.py build
```

## Usage

```c
#include "nanortc.h"

nano_rtc_t rtc;
nano_rtc_config_t cfg = {
    .crypto = nano_crypto_mbedtls(),
    .role   = NANO_ROLE_CONTROLLED,
};
nano_rtc_init(&rtc, &cfg);

// Exchange SDP via your signaling channel
char answer[2048];
nano_accept_offer(&rtc, remote_offer, answer, sizeof(answer), NULL);

// Event loop (your application drives this)
for (;;) {
    nano_output_t out;
    while (nano_poll_output(&rtc, &out) == NANO_OK) {
        if (out.type == NANO_OUTPUT_TRANSMIT)
            sendto(fd, out.transmit.data, out.transmit.len, ...);
        else if (out.type == NANO_OUTPUT_EVENT)
            handle_event(&out.event);
    }

    // Wait for network data or timeout, then:
    nano_handle_receive(&rtc, now_ms, buf, len, &src);
    // or on timeout:
    nano_handle_timeout(&rtc, now_ms);
}
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
examples/                   Linux application templates
  common/                   Reusable event loop, signaling, media source
  linux_datachannel/        DataChannel echo server
  linux_media_send/         H.264/Opus sender from sample files
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
- **Continuous verification** — `./scripts/ci-check.sh` runs the same checks locally that run in GitHub Actions

The repository structure itself is designed for agent legibility: [AGENTS.md](AGENTS.md) serves as the entry point, with progressive disclosure into deeper documentation. Constraints are enforced by code, not by convention.

## Documentation

| Document | Description |
|----------|------------|
| [AGENTS.md](AGENTS.md) | Agent entry point — build commands, mandatory rules |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Module dependency graph, layer model, data flow |
| [Design Specification](docs/design-docs/nanortc-design-draft.md) | Full authoritative design reference |
| [Core Beliefs](docs/design-docs/core-beliefs.md) | Non-negotiable design principles |
| [Execution Plans](docs/PLANS.md) | Active and completed implementation plans |
| [Quality Score](docs/QUALITY_SCORE.md) | Per-module quality grades |
| [RFC Index](docs/references/rfc-index.md) | Protocol specification references |

## Contributing

NanoRTC is in early development (Phase 0 — skeleton complete, Phase 1 — DataChannel in progress).

Contributions welcome. Please read [AGENTS.md](AGENTS.md) for build instructions and mandatory rules before submitting changes.

## License

[MIT](LICENSE)
