# ARCHITECTURE.md

Top-level architecture map for NanoRTC. Start here to understand how the codebase is organized.

## System Model

NanoRTC is a **Sans I/O state machine**. The entire library is a pure function of its inputs:

```
                         ┌─────────────────────────┐
  Inputs:                │                         │  Outputs:
                         │                         │
  UDP bytes ────────────►│     nano_rtc_t          │──────► bytes to send
  monotonic time ───────►│   (pure state machine)  │──────► application events
  user commands ────────►│                         │──────► next timeout (ms)
                         │  No sockets. No threads.│
                         │  No malloc. No clocks.  │
                         └─────────────────────────┘
```

The caller owns the event loop, sockets, and clock. NanoRTC owns protocol logic only.

## Module Dependency Graph

Dependencies flow strictly downward. No cycles allowed.

```
                    ┌──────────┐
                    │ nano_rtc │  (main FSM, dispatches to all modules)
                    └────┬─────┘
           ┌─────────────┼──────────────┬──────────────┐
           ▼             ▼              ▼              ▼
      ┌─────────┐  ┌──────────┐  ┌───────────┐  ┌─────────┐
      │nano_sdp │  │ nano_ice │  │ nano_dtls │  │nano_sctp│
      └─────────┘  └────┬─────┘  └─────┬─────┘  └────┬────┘
                        ▼              │              ▼
                   ┌──────────┐        │     ┌────────────────┐
                   │nano_stun │        │     │nano_datachannel│
                   └──────────┘        │     └────────────────┘
                                       │
                              ┌────────┴────────┐
                              ▼                 ▼
                    ┌──────────────┐    ┌──────────────┐
                    │  nano_srtp   │    │   nano_rtp   │  ← AUDIO or VIDEO
                    └──────────────┘    └──────┬───────┘
                                               │
                                    ┌──────────┼──────────┐
                                    ▼          ▼          ▼
                              ┌──────────┐ ┌────────┐ ┌────────┐
                              │nano_rtcp │ │ jitter │ │  bwe   │ ← AUDIO / VIDEO
                              └──────────┘ └────────┘ └────────┘

  Cross-cutting:
  ┌──────────────────┐   ┌──────────────┐
  │ nano_crypto.h    │   │ nano_crc32c  │
  │ (provider iface) │   │ (SCTP csum)  │
  └──────────────────┘   └──────────────┘
```

## Layer Model

Within the library, code is organized in strict layers:

| Layer | Files | Rule |
|-------|-------|------|
| **Configuration** | `include/nanortc_config.h` | Compile-time tunables with `#ifndef` defaults. User overrides via `NANORTC_CONFIG_FILE` or ESP-IDF Kconfig. |
| **Public API** | `include/nanortc.h` | Only file users `#include`. Defines all public types and functions. |
| **State Machine** | `src/nano_rtc.c`, `src/nano_rtc_internal.h` | Orchestrates all modules. Only file that touches all subsystems. |
| **Protocol Modules** | `src/nano_*.c` + `src/nano_*.h` | Each module owns one protocol. Communicates via return values and caller buffers — no callbacks between modules. |
| **Crypto Interface** | `crypto/nano_crypto.h` | Abstract boundary. Protocol modules call this, never mbedtls directly. |
| **Crypto Provider** | `crypto/nano_crypto_mbedtls.c` or `nano_crypto_openssl.c` | Concrete implementation. Selected at build time via `-DNANORTC_CRYPTO=`. |

**Dependency rules:**
- Public API → State Machine → Protocol Modules → Crypto Interface → Crypto Provider
- Protocol modules may depend on each other only as shown in the dependency graph
- No module may include OS/platform headers (enforced by CI)
- No module may call malloc (enforced by CI)

## Feature Flags

Orthogonal compile-time feature flags control which modules are included:

| Feature flag | Modules compiled | Guard macro |
|---|---|---|
| *(core, always)* | rtc, ice, stun, dtls, sdp, crc32 | — |
| `NANO_FEATURE_DATACHANNEL` | sctp, datachannel, crc32c | `#if NANO_FEATURE_DATACHANNEL` |
| `NANO_FEATURE_AUDIO` or `VIDEO` | rtp, rtcp, srtp | `#if NANO_HAVE_MEDIA_TRANSPORT` |
| `NANO_FEATURE_AUDIO` | jitter | `#if NANO_FEATURE_AUDIO` |
| `NANO_FEATURE_VIDEO` | bwe | `#if NANO_FEATURE_VIDEO` |

Sub-features (only when `DATACHANNEL=1`):
- `NANO_FEATURE_DC_RELIABLE` — retransmit/RTO logic (default ON)
- `NANO_FEATURE_DC_ORDERED` — SSN-based ordered delivery (default ON)

Six CI-tested combinations: DATA, AUDIO, MEDIA, AUDIO_ONLY, MEDIA_ONLY, CORE_ONLY.

## Data Flow (packet lifecycle)

### Inbound (UDP → application event)

```
nano_handle_receive(rtc, now_ms, data, len, src)
  │
  ├── byte[0] ∈ [0x00-0x03] → nano_stun → nano_ice
  │                                          ├── (controlled) respond with Binding Response
  │                                          ├── (controlling) process Binding Response
  │                                          └── ICE connected event
  │
  ├── byte[0] ∈ [0x14-0x40] → nano_dtls
  │                              ├── handshake → DTLS connected event
  │                              └── app data → nano_sctp
  │                                               └── nano_datachannel
  │                                                    └── DC data/string event
  │
  └── byte[0] ∈ [0x80-0xBF] → nano_srtp → nano_rtp  (AUDIO/MEDIA only)
                                             ├── nano_jitter → audio/video event
                                             └── nano_rtcp feedback
```

### Outbound (application → UDP)

```
nano_poll_output(rtc, &out)
  │
  ├── NANO_OUTPUT_TRANSMIT → caller does sendto()
  ├── NANO_OUTPUT_EVENT    → caller processes event
  └── NANO_OUTPUT_TIMEOUT  → caller sets select() timeout
```

## Key Files

| Purpose | Path |
|---------|------|
| Configuration defaults | `include/nanortc_config.h` |
| Public API | `include/nanortc.h` |
| Main state machine | `src/nano_rtc.c` |
| Internal state struct | `src/nano_rtc_internal.h` |
| Crypto provider interface | `crypto/nano_crypto.h` |
| Design document (authoritative) | `docs/design-docs/nanortc-design-draft.md` |
| Quality tracking | `docs/QUALITY_SCORE.md` |
| Active execution plans | `docs/exec-plans/active/` |
| Development workflow | `docs/engineering/development-workflow.md` |
| Architecture constraints | `docs/engineering/architecture-constraints.md` |
