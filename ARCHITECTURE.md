# ARCHITECTURE.md

Top-level architecture map for NanoRTC. Start here to understand how the codebase is organized.

## System Model

NanoRTC is a **Sans I/O state machine**. The entire library is a pure function of its inputs:

```
                         ┌─────────────────────────┐
  Inputs:                │                         │  Outputs:
                         │                         │
  UDP bytes ────────────►│     nanortc_t           │──────► bytes to send
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
                              ┌──────────┐ ┌────────┐ ┌────────┐ ┌──────────┐
                              │nano_rtcp │ │ jitter │ │  bwe   │ │nano_h264 │
                              └──────────┘ └────────┘ └────────┘ └──────────┘
                                            ↑ AUDIO    ↑ VIDEO    ↑ VIDEO

  TURN relay (optional, controlled by NANORTC_FEATURE_TURN):
  ┌──────────┐
  │nano_turn │  (TURN client: Allocate/Refresh/Permission/ChannelBind/Send/Data)
  └──────────┘

  Cross-cutting:
  ┌──────────────────┐   ┌──────────────┐   ┌────────────┐
  │ nanortc_crypto.h │   │ nano_crc32c  │   │ nano_addr  │
  │ (provider iface) │   │ (SCTP csum)  │   │ (IP parse) │
  └──────────────────┘   └──────────────┘   └────────────┘
```

## Layer Model

Within the library, code is organized in strict layers:

| Layer | Files | Rule |
|-------|-------|------|
| **Configuration** | `include/nanortc_config.h` | Compile-time tunables with `#ifndef` defaults. User overrides via `NANORTC_CONFIG_FILE` or ESP-IDF Kconfig. |
| **Public API** | `include/nanortc.h` | Only file users `#include`. Defines all public types, `struct nanortc` layout (for stack allocation), and functions. |
| **State Machine** | `src/nano_rtc.c` | Orchestrates all modules. Internal helpers: `rtc_generate_ice_credentials`, `rtc_apply_remote_sdp`, `rtc_apply_negotiated_media`, `rtc_add_sdp_candidates`, `rtc_drain_dtls_output`, `rtc_emit_event`, `direction_complement`. |
| **Protocol Modules** | `src/nano_*.c` + `src/nano_*.h` | Each module owns one protocol. Communicates via return values and caller buffers — no callbacks between modules. |
| **Crypto Interface** | `crypto/nanortc_crypto.h` | Abstract boundary. Protocol modules call this, never mbedtls directly. |
| **Crypto Provider** | `crypto/nanortc_crypto_mbedtls.c` or `nanortc_crypto_openssl.c` | Concrete implementation. Selected at build time via `-DNANORTC_CRYPTO=`. |

**Dependency rules:**
- Public API → State Machine → Protocol Modules → Crypto Interface → Crypto Provider
- Protocol modules may depend on each other only as shown in the dependency graph
- No module may include OS/platform headers (enforced by CI)
- No module may call malloc (enforced by CI)

## Feature Flags

Orthogonal compile-time feature flags control which modules are included:

| Feature flag | Modules compiled | Guard macro |
|---|---|---|
| *(core, always)* | rtc, ice, stun, dtls, sdp, crc32, addr | — |
| `NANORTC_FEATURE_DATACHANNEL` | sctp, datachannel, crc32c | `#if NANORTC_FEATURE_DATACHANNEL` |
| `NANORTC_FEATURE_AUDIO` or `VIDEO` | rtp, rtcp, srtp | `#if NANORTC_HAVE_MEDIA_TRANSPORT` |
| `NANORTC_FEATURE_AUDIO` | jitter | `#if NANORTC_FEATURE_AUDIO` |
| `NANORTC_FEATURE_VIDEO` | h264, bwe | `#if NANORTC_FEATURE_VIDEO` |
| `NANORTC_FEATURE_IPV6` | IPv6 parsing/formatting in addr | `#if NANORTC_FEATURE_IPV6` |

Sub-features (only when `DATACHANNEL=1`):
- `NANORTC_FEATURE_DC_RELIABLE` — retransmit/RTO logic (default ON)
- `NANORTC_FEATURE_DC_ORDERED` — SSN-based ordered delivery (default ON)

`NANORTC_FEATURE_IPV6` (default ON) controls IPv6 address string parsing in `nano_addr`. When OFF, IPv6 candidates are silently rejected. IPv4 parsing is always compiled.

Six CI-tested combinations: DATA, AUDIO, MEDIA, AUDIO_ONLY, MEDIA_ONLY, CORE_ONLY.

## Data Flow (packet lifecycle)

### Inbound (UDP → application event)

```
nanortc_handle_input(rtc, &(nanortc_input_t){.now_ms, .data, .len, .src, .dst})
  │
  ├── byte[0] ∈ [0x40-0x7F] → nano_turn ChannelData unwrap
  │                              └── re-dispatch inner packet with peer address
  │
  ├── byte[0] ∈ [0x00-0x03] → STUN demux:
  │   ├── from TURN server → nano_turn (Data indication unwrap / response handling)
  │   ├── from STUN server → srflx discovery (XOR-MAPPED-ADDRESS → trickle candidate)
  │   └── from peer        → nano_ice
  │                            ├── (controlled) respond with Binding Response
  │                            ├── (controlling) process Binding Response
  │                            └── ICE connected event
  │
  ├── byte[0] ∈ [0x14-0x3F] → nano_dtls
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
nanortc_poll_output(rtc, &out)
  │
  ├── NANORTC_OUTPUT_TRANSMIT → caller does sendto()
  │     (if ICE selected pair is RELAY and TURN is allocated: lazy wrap into
  │      ChannelData/Send indication using rtc->turn_buf, dest rewritten to
  │      TURN server; otherwise dest = peer directly)
  ├── NANORTC_OUTPUT_EVENT    → caller processes event
  └── NANORTC_OUTPUT_TIMEOUT  → caller sets select() timeout
```

The TURN wrap is **deferred** to `nanortc_poll_output()` rather than done at
enqueue time: `rtc_enqueue_transmit()` stamps a `via_turn` flag + the original
peer destination in a per-slot `out_wrap_meta[]` side-table and stores the
unwrapped data. This avoids the eager-wrap collision a burst of N media
packets would have caused into a single shared scratch buffer (each
`nanortc_output_t` slot is just a pointer; eager wraps would all alias the
last writer). The receive-side `via_turn` signal is plumbed through
`rtc_process_receive` → `ice_handle_stun` so the controlled side correctly
flips `selected_type=RELAY` when a USE-CANDIDATE check arrives unwrapped from
a TURN Data Indication / ChannelData — see [docs/engineering/turn-rfc-compliance.md](docs/engineering/turn-rfc-compliance.md)
Phase 5.2.

### NAT Traversal (ICE candidate types)

NanoRTC supports all three ICE candidate types (RFC 8445 §5.1.2.1):

| Type | Source | Priority | Discovery |
|------|--------|----------|-----------|
| **host** | Local address from `nanortc_add_local_candidate()` | 2130706431–2130705919 (varies by index) | Caller provides |
| **srflx** | STUN server Binding Response (XOR-MAPPED-ADDRESS) | 1090519295 | Automatic via `stun:` URL |
| **relay** | TURN server Allocate Response (XOR-RELAYED-ADDRESS) | 16777215 | Automatic via `turn:` URL |

Multiple local host candidates are supported (`NANORTC_MAX_LOCAL_CANDIDATES`, default 4).
Each host candidate gets a distinct priority per RFC 8445 §5.1.2.1:
`priority = (126 << 24) | ((65535 - index) << 8) | 255`.

Timer-driven lifecycle in `rtc_process_timers()`:
- **STUN srflx**: Simple Binding Request → retry 3× at 500ms → extract mapped address
- **TURN relay**: Allocate → 401 challenge → authenticated retry → Refresh (10min) + CreatePermission (5min) + ChannelBind (10min)

## Key Files

| Purpose | Path |
|---------|------|
| Configuration defaults | `include/nanortc_config.h` |
| Public API | `include/nanortc.h` |
| Main state machine | `src/nano_rtc.c` |
| Address utilities (IPv4/IPv6) | `src/nano_addr.c` |
| Crypto provider interface | `crypto/nanortc_crypto.h` |
| Design document (authoritative) | `docs/design-docs/nanortc-design-draft.md` |
| Quality tracking | `docs/QUALITY_SCORE.md` |
| Active execution plans | `docs/exec-plans/active/` |
| Interop tests (libdatachannel) | `tests/interop/` |
| Development workflow | `docs/engineering/development-workflow.md` |
| Architecture constraints | `docs/engineering/architecture-constraints.md` |
