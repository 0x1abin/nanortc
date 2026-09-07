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
                    ┌──────────────────────────────────────────────────────┐
                    │                    nano_rtc                          │
                    │  (transport backbone: ICE/TURN/DTLS/SCTP demux,      │
                    │   output queue, timer dispatch, public API)          │
                    │  +  nano_rtc_negotiate (offer/answer surface)        │
                    │  +  nano_rtc_media     (RTP/RTCP/BWE paths)          │
                    └──────────┬───────────────────────────────────────────┘
           ┌─────────────┬─────┴────────┬──────────────┐
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
                          ┌──────────┬─────────┼─────────┬─────────┬──────────┐
                          ▼          ▼         ▼         ▼         ▼          ▼
                    ┌──────────┐ ┌────────┐ ┌──────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐
                    │nano_rtcp │ │ jitter │ │ bwe  │ │ nano_twcc│ │nano_h264 │ │nano_h265 │
                    └──────────┘ └────────┘ └──────┘ └──────────┘ └──────────┘ └──────────┘
                                  ↑ AUDIO    ↑VIDEO   ↑ VIDEO       ↑ VIDEO     ↑ H265

  TURN relay (optional, controlled by NANORTC_FEATURE_TURN):
  ┌──────────┐
  │nano_turn │  (TURN client: Allocate/Refresh/Permission/ChannelBind/Send/Data)
  └──────────┘

  Cross-cutting (always compiled):
  ┌──────────────────┐ ┌─────────────┐ ┌───────────┐ ┌──────────┐
  │ nanortc_crypto.h │ │ nano_crc32c │ │ nano_addr │ │ nano_log │
  │ (provider iface) │ │ (SCTP csum) │ │ (IP parse)│ │ (callback│
  │                  │ │             │ │           │ │  inject) │
  └──────────────────┘ └─────────────┘ └───────────┘ └──────────┘

  Media-only utilities (compiled with NANORTC_HAVE_MEDIA_TRANSPORT):
  ┌────────────┐ ┌──────────────┐ ┌──────────────────────────┐
  │ nano_media │ │ nano_annex_b │ │ nano_base64 (H265 sprop) │
  │ (track ops)│ │ (NAL scanner)│ │                          │
  └────────────┘ └──────────────┘ └──────────────────────────┘
```

## Layer Model

Within the library, code is organized in strict layers:

| Layer | Files | Rule |
|-------|-------|------|
| **Configuration** | `include/nanortc_config.h` | Compile-time tunables with `#ifndef` defaults. User overrides via `NANORTC_CONFIG_FILE` or ESP-IDF Kconfig. |
| **Public API** | `include/nanortc.h` | Only file users `#include`. Defines all public types, `struct nanortc` layout (for stack allocation), and functions. |
| **State Machine** | `src/nano_rtc.c` + `src/nano_rtc_negotiate.c` + `src/nano_rtc_media.c` | Orchestrates all modules across three TUs sharing the private interface in `src/nano_rtc_internal.h`. `nano_rtc.c` owns the transport backbone (ICE/TURN/DTLS/SCTP demux, output queue, timer dispatch, public API entry points); `nano_rtc_negotiate.c` owns offer/answer + iceServers helpers; `nano_rtc_media.c` owns RTP/RTCP/BWE paths and is compiled only under `NANORTC_HAVE_MEDIA_TRANSPORT`. |
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
| *(core, always)* | rtc, rtc_negotiate, ice, stun, dtls, sdp, crc32, addr, log | — |
| `NANORTC_FEATURE_DATACHANNEL` | sctp, datachannel, crc32c | `#if NANORTC_FEATURE_DATACHANNEL` |
| `NANORTC_FEATURE_AUDIO` or `VIDEO` | rtp, rtcp, srtp, media, rtc_media | `#if NANORTC_HAVE_MEDIA_TRANSPORT` |
| `NANORTC_FEATURE_AUDIO` | jitter | `#if NANORTC_FEATURE_AUDIO` |
| `NANORTC_FEATURE_VIDEO` | h264, annex_b, bwe, twcc | `#if NANORTC_FEATURE_VIDEO` |
| `NANORTC_FEATURE_H265` | h265, base64 (requires VIDEO) | `#if NANORTC_FEATURE_H265` |
| `NANORTC_FEATURE_TURN` | turn | `#if NANORTC_FEATURE_TURN` |
| `NANORTC_FEATURE_IPV6` | IPv6 parsing/formatting in addr | `#if NANORTC_FEATURE_IPV6` |
| `NANORTC_FEATURE_ICE_SRFLX` | srflx candidate gathering in ice | `#if NANORTC_FEATURE_ICE_SRFLX` |

Sub-features (only when `DATACHANNEL=1`):
- `NANORTC_FEATURE_DC_RELIABLE` — retransmit/RTO logic (default ON)
- `NANORTC_FEATURE_DC_ORDERED` — SSN-based ordered delivery (default ON)

`NANORTC_FEATURE_IPV6` (default ON) controls IPv6 address string parsing in `nano_addr`. When OFF, IPv6 candidates are silently rejected. IPv4 parsing is always compiled.

Seven CI-tested combinations: DATA, AUDIO, MEDIA, MEDIA_H265, AUDIO_ONLY, MEDIA_ONLY, CORE_ONLY.

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
                                             ├── (video) forward RTP seq gap →
                                             │     debounced auto-PLI keyframe request
                                             └── nano_rtcp feedback
```

**Receive-side loss recovery (video).** Each inbound video RTP packet's sequence
number is checked against the track's last in-order seq (`NANORTC_FEATURE_VIDEO_AUTO_PLI`,
default on). A forward gap — a lost packet — triggers a debounced RTCP PLI
(`NANORTC_VIDEO_PLI_MIN_INTERVAL_MS`) so the sender re-sends a keyframe and the
decoder resyncs, instead of the receiver freezing until the app asks. The
emitted `NANORTC_EV_MEDIA_DATA.contiguous` flag reflects whether a gap preceded
the frame. Late/reordered packets are ignored by the gap test. An optional bounded receive
reorder buffer (`NANORTC_FEATURE_VIDEO_REORDER`, `nano_reorder.c`, **opt-in /
default off**) sits in front of the depacketizer: it releases packets strictly
in seq order — healing benign WiFi/cellular reordering before it breaks FU
reassembly — and makes the loss signal *precise* (its skip, not a raw seq gap,
is the only loss source, so reordering stops false-firing auto-PLI). It is
latency-capped (`NANORTC_VIDEO_REORDER_MAX_WAIT_MS`) and costs `SLOTS ×
NANORTC_MEDIA_BUF_SIZE` per video track, so a send-only camera leaves it off.

### Outbound (application → UDP)

```
nanortc_poll_output(rtc, &out)
  │
  ├── (video pacer pump, if NANORTC_FEATURE_VIDEO_PACING) releases due video
  │     RTP fragments from the pkt_ring pace FIFO into out_queue at the
  │     BWE-derived rate before the queue is drained
  │
  ├── NANORTC_OUTPUT_TRANSMIT → caller does sendto()
  │     (if ICE selected pair is RELAY and TURN is allocated: lazy wrap into
  │      ChannelData/Send indication using rtc->turn_buf, dest rewritten to
  │      TURN server; otherwise dest = peer directly)
  └── NANORTC_OUTPUT_EVENT    → caller processes event

nanortc_next_timeout_ms(rtc, now_ms, &wait_ms) → caller sets select() timeout
```

**Bounded ownership.** SCTP copies inbound chunks into its receive pool and
releases complete messages one at a time during polling; incomplete B/E fragments
are never delivered. Contiguous acknowledged fragments are merged in place, so
small peer fragments do not consume one descriptor each for the whole message.
One fragment-identity predicate drives bounded iterative merging and delivery.
SCTP returns a caller-owned message view; RTC emits it directly as a DataChannel
event. DCEP keeps only per-channel pending state: peek into caller scratch and
commit only after SCTP admission, with no global output cache. Protocol and
retry metadata remain per channel and are exposed on OPEN events. Explicit
`partial_reliability=true` distinguishes zero retries from the reliable default;
timeout and gap-SACK policies share whole-message abandonment.
Sends reserve the complete message and all fragment descriptors before committing.
The SDP receive limit is `NANORTC_SCTP_MAX_MESSAGE_SIZE` (host default 4096 B,
trimmed to the receive-pool limit by ESP profiles); sends also honor the peer's
`max-message-size`. Permanent size errors differ from temporary `WOULD_BLOCK`.

Candidates retain their source identity and format their strings only on poll.
When the output ring fills, scalar notifications are retained and coalesced by
event type and track/channel; repeated state updates keep their latest snapshot.
DataChannel payloads remain in the SCTP receive pool until polled; realtime
media follows its track admission/playout policy. Public pointers
are still valid only until the next state mutation on that instance.

**TURN responsibilities.** TURN output and timeout queries share one task
selector; an outstanding request contributes its retransmission deadline, not
an expired refresh deadline. RTC selects candidate order and reserves TX slots;
TURN owns permission/channel readiness and retry/backoff. Response authentication
precedes error dispatch. Nonce rotation retires affected transaction IDs while
preserving retry budgets and concurrent deadlines. Polling drains producers
iteratively, preserving output priority and borrowed-pointer lifetime.

**Clock and logging.** DTLS providers expose set-time, next-timeout and
handle-timeout operations. mbedTLS runs retransmissions from the caller clock;
OpenSSL 3.0 exposes its internal real-clock deadline through the adapter. Timer
and packet paths share handshake completion/key-export handling. Audio jitter
playout participates in the deadline query. Logs use `rtc->config.log` explicitly;
stateless codecs have no ambient callback or mutable singleton.

**Send pacing (video).** Video RTP egress is metered by a Sans-I/O leaky token
bucket (`NANORTC_FEATURE_VIDEO_PACING`, default on). A multi-fragment IDR is
spread across up to one frame interval at `BWE_estimate × NANORTC_PACING_FACTOR_PCT`
instead of bursting onto the wire and overrunning the network bottleneck buffer
(self-inflicted loss → PLI → larger IDR). The fragments stage in the existing
`pkt_ring` (a `[head, tail)` pace FIFO over the same slots the NACK history
uses); `nano_rtc_pacer_pump()` releases the due ones into `out_queue` at the top
of `nanortc_poll_output()`, and `nanortc_next_timeout_ms()` reports the next
release deadline so the caller's loop wakes on schedule. A backlog older than
`NANORTC_PACING_MAX_QUEUE_MS` is flushed immediately (catch-up), capping the
latency the pacer can ever add. NACK retransmits, RTCP/PLI, control packets and
audio bypass the pacer. The atomic frame-admission gate accounts for the pace
backlog so a frame still ships whole or returns `WOULD_BLOCK` — it never
truncates. This is the egress complement to the admission gate added in #67.

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

NanoRTC gathers host, srflx and relay candidates and can learn remote prflx candidates from authenticated checks (RFC 8445 §7.3.1.3):

| Type | Source | Priority | Discovery |
|------|--------|----------|-----------|
| **host** | Local address from `nanortc_add_local_candidate()` | 2130706431–2130705919 (varies by index) | Caller provides |
| **srflx** | STUN server Binding Response (XOR-MAPPED-ADDRESS) | `ICE_SRFLX_PRIORITY(index)` | Automatic via `stun:` URL |
| **relay** | TURN server Allocate Response (XOR-RELAYED-ADDRESS) | 16777215 | Automatic via `turn:` URL |

Both roles initiate ordinary and triggered checks. The controlling role validates
before nominating; the controlled role completes nomination after its reverse
check succeeds. RTC supplies TURN permission readiness and transmit capacity;
ICE supplies the selected task, its actual pair and its deadline. srflx traffic
uses the retained host base as its source hint. See
[Issue #81 implementation](docs/engineering/issue-81-ice.md) for bounded-state
behavior, remaining RFC gaps and validation.

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
| Main state machine (transport backbone) | `src/nano_rtc.c` |
| SDP negotiation surface | `src/nano_rtc_negotiate.c` |
| Media path orchestration (RTP/RTCP/BWE) | `src/nano_rtc_media.c` |
| Internal interface shared by the three orchestration TUs | `src/nano_rtc_internal.h` |
| Address utilities (IPv4/IPv6) | `src/nano_addr.c` |
| Crypto provider interface | `crypto/nanortc_crypto.h` |
| Design document (authoritative) | `docs/design-docs/nanortc-design-draft.md` |
| Quality tracking | `docs/QUALITY_SCORE.md` |
| Active execution plans | `docs/exec-plans/active/` |
| Interop tests (libdatachannel) | `tests/interop/` |
| Development workflow | `docs/engineering/development-workflow.md` |
| Architecture constraints | `docs/engineering/architecture-constraints.md` |
