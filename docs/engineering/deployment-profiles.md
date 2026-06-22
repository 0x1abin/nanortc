# Deployment Profiles — recommended feature configuration

NanoRTC ships the **heavier receive-side** loss-recovery primitives — the reorder
buffer, receiver NACK, and FEC (`NANORTC_FEATURE_VIDEO_REORDER` / `_NACK_RX` /
`_FEC`) — **off by default** so the baseline build stays minimal (a
memory-constrained send-only camera shouldn't pay for a receiver's reorder
buffer). The lightweight, always-on robustness — the **send pacer** and
**auto-PLI** keyframe recovery — is **default-on** (negligible cost; detailed
below). "Most suitable for embedded real-time video" means *providing* the
primitives and making the right configuration obvious — not forcing every buffer
on every device. This page maps the three real deployment shapes to the feature
flags to enable.

The always-compiled, zero-config baseline already includes: the **send pacer**
(`NANORTC_FEATURE_VIDEO_PACING`, default **on** — anti-burst, no cost when idle),
**inbound PLI handling** (a browser's PLI → `NANORTC_EV_KEYFRAME_REQUEST`, so the
app can re-IDR), **auto-PLI** keyframe recovery (`NANORTC_FEATURE_VIDEO_AUTO_PLI`,
default **on** — cheap gap→debounced-PLI), and BWE (REMB + TWCC) feeding the
encoder loop. So even the baseline recovers from loss via keyframes.

## Measured footprint (`sizeof(nanortc_t)`, host x86-64, default config sizes)

Hard numbers — the central "suitable for embedded" axis. Measured by compiling
`sizeof(nanortc_t)` per feature combo (one connection's entire state; no malloc,
so this *is* the per-connection RAM):

| Build | `sizeof(nanortc_t)` | Δ vs prior |
|---|---:|---:|
| CORE_ONLY (no DC/audio/video) | ~14.5 KB | — |
| **MEDIA default** (DC + audio + video, pacer + auto-PLI on) | **~104 KB** | baseline |
| + REORDER | ~124 KB | **+20 KB** (≈10 KB/video track × `MAX_MEDIA_TRACKS`) |
| + NACK_RX | ~124 KB | +0.1 KB (negligible) |
| + FEC | **~150 KB** | **+27 KB** (send group + FEC tx ring + receive ring + coordination/adaptive state, K=8) |

Takeaways: the baseline media stack is ~104 KB and already recovers via keyframes
(pacer + auto-PLI). The heavy buffers are strictly opt-in — REORDER (+20 KB) and
FEC (+23 KB) are the cost of the receive-robustness / proactive-recovery
primitives, paid only when a profile below enables them. A send-only camera
(Profile A) stays near the 104 KB baseline. (On-target ESP32/rk3588 sizes differ
with smaller config defaults — `NANORTC_VIDEO_PKT_RING_SIZE`, `MEDIA_BUF`, etc.;
see [memory-profiles.md](memory-profiles.md).)

### Code size vs alternatives (footprint = the embedded axis)

Measured via the `benchmarks/` size harness (`compare_sizes.sh`) for nanortc, and
`size`(1) `.text` of the other stacks' built archives (arm64 Darwin, `-O2`,
OpenSSL — which all three share, so OpenSSL is excluded from the dep counts):

| Stack | code (`.text`) | external deps | language / model |
|---|---:|---|---|
| **nanortc** (whole MEDIA stack) | **~91 KB** | **1** (OpenSSL) | pure C99, no heap, no threads, Sans-I/O |
| **KVS WebRTC SDK** (Amazon's embedded C SDK) — *dependency libs only* | **~1.3 MB** | 4+ (usrsctp 692 KB, libwebsockets 292 KB, libsrtp2, kvspic producer ~255 KB) | C, malloc + threads + libwebsockets event loop |
| libdatachannel + libjuice + libsrtp2 + libusrsctp | ~3.6 MB | 3 bundled + C++ runtime | C++17 (STL / exceptions / threads) |

The **KVS comparison is the apt one** — both are C, both target embedded. nanortc's
*entire* self-contained 91 KB stack (its own SCTP, SRTP, STUN, ICE, SDP, DTLS
glue) is **~14× smaller than just KVS's dependency libraries**, before KVS's own
~24.7 K-SLOC WebRTC client is added. The reason is architectural and is what
actually decides embedded fit: nanortc bundles minimal in-house SCTP/SRTP/STUN/ICE
and is **Sans-I/O with no heap, no threads, and no event-loop dependency**,
whereas KVS pulls in `libwebsockets` (signaling transport) + `usrsctp` (data
channels) and assumes malloc + threads — heavy for a tight MCU. (Sizes are `.a`
`.text` pre-link-strip → order-of-magnitude, not exact stripped-binary deltas;
the KVS client lib was not built here, so its figure is deps-only and thus a
*lower bound* on the gap.)

nanortc SLOC: ~21 K (27 `.c` + 25 `.h`).

### Per-packet CPU hot path (`benchmarks/bench_rtp`, `bench_srtp`, host arm64, -O2, software crypto)

| Hot path (1200 B MTU packet) | per packet | rate |
|---|---:|---:|
| RTP pack (header build, zero-copy) | ~20 ns | 49 M pkt/s |
| RTP unpack | ~3 ns | 373 M pkt/s |
| SRTP protect (AES-128-CM + HMAC-SHA1-80) | **~1.35 µs** | 741 K pkt/s |
| SRTP unprotect | ~2.7 µs | 375 K pkt/s |

The actionable result: **the protocol plumbing is essentially free (~20 ns/packet);
per-packet CPU is dominated by SRTP crypto (~1.35 µs), ~67× the RTP cost.** So on
an MCU the per-packet budget is *crypto + encoder*, not nanortc's framing — which
is why a chip with a **hardware AES engine (ESP32, rk3588)** matters far more than
the stack's own efficiency, and why the always-allocation-free, no-copy hot path
is the right design (it adds nothing measurable on top of the unavoidable crypto).
At 30 fps × ~50 pkt/frame ≈ 1500 pkt/s, software SRTP is ~2 ms CPU/s on this host —
negligible; HW-AES MCUs drop it further. (Host arm64 numbers — indicative of the
*shape* of the cost, not on-target absolute cycles; real MCU latency/CPU/power
still need on-device measurement, as does loss-resilience under a real link.)

## Profile A — Embedded camera (send-only video)  ← the flagship

The camera sends H.264/H.265 to a browser; it receives no video. Inbound
video-recovery features are inert here (no inbound stream to repair), so leave
them off and save the RAM.

| Feature | Setting | Why |
|---|---|---|
| `NANORTC_FEATURE_VIDEO_PACING` | **on** (default) | Spread IDR bursts over the uplink → no self-inflicted loss. The single biggest uplink-quality lever. |
| `NANORTC_FEATURE_VIDEO_AUTO_PLI` | on (default) | Inert for send-only (no inbound gaps); harmless. |
| `NANORTC_FEATURE_VIDEO_REORDER` | **off** | No inbound video to reorder; saves ~10 KB/track. |
| `NANORTC_FEATURE_VIDEO_NACK_RX` | **off** | Receiver feature; no inbound video. (The camera still *answers* a browser's NACK via its pkt_ring retransmit — always compiled.) |
| FEC (Phase 13) | optional send-side | Proactive zero-RTT recovery for high-RTT/high-loss uplinks. **Adaptive by default** (`NANORTC_FEC_ADAPTIVE`): the group size K tracks the smoothed TWCC loss — **no FEC overhead on a clean link**, more protection (smaller K) only when the uplink is actually lossy — so a camera can leave it on without paying redundancy when the link is good. |

RAM: the leanest media profile. Drive the encoder from `NANORTC_EV_BITRATE_ESTIMATE`
(the example `bwe_coordinator`).

## Profile B — Viewer (recv-only video)  ← doorbell screen, monitor

The device receives video from a camera/browser over WiFi/cellular. This is where
the receive-robustness features earn their RAM.

| Feature | Setting | Why |
|---|---|---|
| `NANORTC_FEATURE_VIDEO_REORDER` | **on** | Heal WiFi/cellular reordering before it breaks FU reassembly; makes the loss signal precise. Tune `NANORTC_VIDEO_REORDER_MAX_WAIT_MS` ≥ link RTT if pairing with NACK. |
| `NANORTC_FEATURE_VIDEO_NACK_RX` | **on** | Retransmit-recover small losses (cheaper than a keyframe). Most effective *with* the reorder buffer holding the gap. |
| `NANORTC_FEATURE_VIDEO_AUTO_PLI` | on (default) | Fallback when a loss is unrecoverable by NACK/FEC. |
| FEC (Phase 13) | optional recv-side | Add when RTT is too high for NACK to beat the playout deadline, or loss is high enough that NACK retransmits are themselves lost. |

RAM: baseline + `NANORTC_VIDEO_REORDER_SLOTS × NANORTC_MEDIA_BUF_SIZE` (~10 KB at
8 slots) per video track. Shrink `NANORTC_VIDEO_REORDER_SLOTS` on tight SRAM.

## Profile C — Two-way intercom (send + receive video)

Union of A and B: pacing + auto-PLI (baseline) **plus** reorder + NACK_RX for the
inbound stream. Budget both the send pkt_ring and the receive reorder buffer.
Watch `stats_auto_pli_sent` / `stats_nack_sent` / `stats_pace_catchup` to tune.

## Decision factors

- **RAM headroom** → gate `NANORTC_FEATURE_VIDEO_REORDER` (its buffer dominates).
- **Link RTT vs jitter/playout budget** → if RTT > budget, NACK is too late; lean
  on FEC (Phase 13) and a larger reorder cap.
- **Loss rate** → low loss: NACK suffices (cheap); high loss: add FEC (NACK
  retransmits also get lost).
- **Uplink scarcity (cameras)** → prefer NACK over FEC (NACK costs bandwidth only
  on loss; FEC costs it always).

## Setting the flags

Host / CMake: `-DCMAKE_C_FLAGS="-DNANORTC_FEATURE_VIDEO_REORDER=1 -DNANORTC_FEATURE_VIDEO_NACK_RX=1"`,
or via `NANORTC_CONFIG_FILE`. ESP-IDF: the matching `CONFIG_NANORTC_*` Kconfig
symbols (mirrored in `nanortc_config.h`). All knobs have `#ifndef` defaults and
compile-time `#error` validation — see [memory-profiles.md](memory-profiles.md)
for the byte costs.
