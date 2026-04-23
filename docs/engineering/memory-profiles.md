# Memory Profiles

Current `sizeof(nanortc_t)` and `libnanortc.a` `.text` for each canonical
feature combination on ESP32-P4 (RISC-V HP, ESP-IDF 5.5 mbedTLS, `-Os` via
`CONFIG_COMPILER_OPTIMIZATION_SIZE=y`). Host (64-bit) sizes are slightly
larger due to pointer/size_t widths; 32-bit ARM targets land within ~5 % of
the ESP32-P4 numbers.

All numbers below come from `./scripts/measure-sizes.sh --esp32 esp32p4`
against the ESP-IDF Kconfig defaults in `Kconfig`. Full ICE stack is
preserved (TURN relay, srflx discovery, IPv6 host candidates, RFC 8445
hardening); only buffer/queue sizing and logging are trimmed for IoT
targets. Host Linux/macOS builds use `nanortc_config.h`'s generous
defaults so interop/fuzz tests keep their timing headroom.

## Configuration Matrix

| Configuration | Flash (.text) | `sizeof(nanortc_t)` | Notes |
|---|---|---|---|
| `CORE_ONLY` (no DC, no media) | 29.0 KB | 10.2 KB | ICE + DTLS + SDP + STUN + TURN |
| `DC-only` | 38.8 KB | 19.4 KB | Adds SCTP + DCEP |
| `Audio only` | 40.8 KB | 20.6 KB | Adds 1 audio track (jitter buffer) |
| `DC + Audio` | 50.6 KB | 29.9 KB | Typical duplex voice IoT config |
| `Media only` (no DC) | 45.3 KB | 51.0 KB | Audio + 1 video track + `pkt_ring` + BWE |
| `DC + Audio + Video` | 55.0 KB | 60.3 KB | Full media stack |

`NANORTC_FEATURE_TURN=0` claws back ~11 KB of flash and ~668 B of RAM from
every row — only valid for deployments that can always reach peers via
host / srflx candidates. Building at `-Og` (ESP-IDF's Kconfig default for
the rest of the firmware) inflates every flash figure by roughly 15 %;
the measurement pins `-Os` via `scripts/esp32-measure/sdkconfig.defaults`.

## ESP-IDF vs host defaults

The Kconfig bakes in the following trims on top of `nanortc_config.h`'s
generous host defaults:

| Knob | Host default | ESP-IDF Kconfig default |
|---|---|---|
| `NANORTC_MAX_DATACHANNELS` | 8 | 2 |
| `NANORTC_MAX_ICE_CANDIDATES` | 8 | 4 |
| `NANORTC_DTLS_BUF_SIZE` | 2048 | 1536 |
| `NANORTC_SDP_BUF_SIZE` | 2048 | 1024 |
| `NANORTC_SCTP_{SEND,RECV,RECV_GAP}_BUF_SIZE` | 4096 each | 2048 each |
| `NANORTC_SCTP_MAX_SEND_QUEUE` | 16 | 4 |
| `NANORTC_SCTP_MAX_RECV_GAP` | 8 | 4 |
| `NANORTC_OUT_QUEUE_SIZE` | 32 | 8 (audio/DC), 16 (video) |
| `NANORTC_VIDEO_PKT_RING_SIZE` | inherits `NANORTC_OUT_QUEUE_SIZE` | inherits `NANORTC_OUT_QUEUE_SIZE` |
| `NANORTC_MEDIA_BUF_SIZE` | 1232 (formula) | 1232 (fixed; `#error` guards `< MTU + 30`) |
| `NANORTC_VIDEO_NAL_BUF_SIZE` | 16384 | 8192 |
| `NANORTC_JITTER_SLOTS` | 32 | 16 |
| `NANORTC_JITTER_SLOT_DATA_SIZE` | 320 | 160 |
| `NANORTC_LOG_LEVEL` | 4 (TRACE) | 1 (WARN) |
| `NANORTC_LOG_NO_LOC` | undefined | defined |

Override any of these via `idf.py menuconfig` or `CONFIG_NANORTC_*` lines
in `sdkconfig.defaults`. If your target has looser constraints (HD video,
high-jitter cellular, large SDPs), raise the knob you care about.

## Biggest Contributors

| Component | Default Size | Tuning Knob |
|---|---|---|
| Jitter buffer (per audio track) | ~11 KB host / ~2.8 KB Kconfig | `NANORTC_JITTER_SLOTS`, `NANORTC_JITTER_SLOT_DATA_SIZE` |
| H.264 NAL reassembly (per video track) | 16 KB host / 8 KB Kconfig | `NANORTC_VIDEO_NAL_BUF_SIZE` |
| Video packet ring (NACK retransmit window) | 39 KB host / ~20 KB Kconfig | `NANORTC_VIDEO_PKT_RING_SIZE` × `NANORTC_MEDIA_BUF_SIZE` |
| SCTP send + recv + gap buffers | ~12 KB host / ~6 KB Kconfig | `NANORTC_SCTP_SEND_BUF_SIZE`, `NANORTC_SCTP_RECV_BUF_SIZE`, `NANORTC_SCTP_RECV_GAP_BUF_SIZE` |
| DTLS buffers (3 × `NANORTC_DTLS_BUF_SIZE`) | 6 KB host / 4.5 KB Kconfig | `NANORTC_DTLS_BUF_SIZE` |
| Shared STUN/RTCP/RTP scratch | 256 B (DC-only) / 1232 B (media) | `NANORTC_STUN_BUF_SIZE` (feature-gated — see below) |
| TURN client | ~668 B | `NANORTC_FEATURE_TURN` (disable only if deployment can always reach peers via host / srflx) |

`NANORTC_MEDIA_BUF_SIZE` has a hard minimum of `NANORTC_VIDEO_MTU + 30 =
1230 B` (RTP header 12 + TWCC extension 8 + MTU payload + SRTP auth tag
10). Dropping below that in a media build is caught at compile time by a
`#error` in `nanortc_config.h`. Default 1232 leaves 2 B headroom;
`examples/esp32_{video,camera}/sdkconfig.defaults` raise it to 1280 to
reserve room for additional RTP header extensions.

### Video NACK ring — tunable independently of the output queue

Since Phase 8 PR-3, `NANORTC_VIDEO_PKT_RING_SIZE` sizes the NACK retransmit
ring independently from `NANORTC_OUT_QUEUE_SIZE`. Default inherits
`NANORTC_OUT_QUEUE_SIZE` to keep existing builds byte-identical. On IoT /
LAN deployments with low packet loss and short retransmit windows,
override to a smaller power of two:

```c
/* Saving is (OUT_QUEUE_SIZE - PKT_RING_SIZE) × MEDIA_BUF_SIZE.
 * Against the host default OUT_QUEUE_SIZE=32 at MEDIA_BUF_SIZE=1232,
 * PKT_RING_SIZE=16 saves ~19 KB. Against the ESP-IDF Kconfig default
 * OUT_QUEUE_SIZE=16 (video), PKT_RING_SIZE=8 saves ~9.6 KB. */
#define NANORTC_VIDEO_PKT_RING_SIZE 16
```

#### Hard sizing rule — not just a drain hint

`out_queue[].transmit.data` stores a *pointer* into `pkt_ring[]`, and
`nanortc_send_video()` invokes the SRTP/RTP packetizer callback for every
FU-A fragment of a single access unit before returning to the caller —
the application has no opportunity to drain in between. Wrapping
`pkt_ring_tail` while earlier-fragment pointers are still pending in
`out_queue` silently overwrites their backing buffers; the receiver sees
the newer fragment's bytes carrying the older fragment's RTP sequence
number, and decoders typically present this as glitched IDRs.

Therefore:

```
NANORTC_VIDEO_PKT_RING_SIZE >= ceil(max_frame_bytes / NANORTC_VIDEO_MTU) + 1
```

Worked numbers at the default `NANORTC_VIDEO_MTU = 1200`:

| Video profile (H.264) | Worst-case IDR | Min `PKT_RING_SIZE` | Round to |
|---|---|---|---|
| 480p, target ≤ 1 Mbps   | ~10 KB | 9  | 16 |
| 720p, target ≤ 4 Mbps   | ~30 KB | 26 | 32 |
| 1080p, target ≤ 8 Mbps  | ~60 KB | 51 | 64 |

Compile-time guards (`#error`) only enforce `power-of-two && >= 4`; the
per-frame bound depends on the encoder's IDR rate-control target and so
must be sized by the integrator. The runtime emits
`NANORTC_LOGW("RTC", "pkt_ring overrun ...")` and bumps
`nanortc_t.stats_pkt_ring_overrun` whenever the ring would wrap before
the caller has drained the prior wave — wire that counter into your
integration smoke tests to catch under-sizing before it reaches the wire.

NACK history depth, in packets, is `PKT_RING_SIZE`. The wall-clock
window depends on stream bitrate and packets-per-frame: at 30 fps a
single-NAL 480p stream gives roughly 500 ms with `PKT_RING_SIZE=16`,
but the same setting on a 720p stream that emits ~26 packets per IDR
covers under 100 ms across IDR boundaries.

### Shared scratch buffer — feature-gated default

`nanortc_t.stun_buf` is a single Sans-I/O scratch region that services, in
time-disjoint phases, STUN request/response encoding, TURN allocate/refresh/
channel framing, RTCP generation and SRTCP protect, and — crucially — the
in-place SRTP unprotect step on the inbound RTP path.

The default size is therefore feature-gated:

- `NANORTC_HAVE_MEDIA_TRANSPORT=0` → 256 B (STUN / RTCP only)
- `NANORTC_HAVE_MEDIA_TRANSPORT=1` → `NANORTC_MEDIA_BUF_SIZE` (1232 B today)

`nanortc_config.h` enforces the invariant with a `#error`:

```c
#if NANORTC_HAVE_MEDIA_TRANSPORT && (NANORTC_STUN_BUF_SIZE < NANORTC_MEDIA_BUF_SIZE)
#error "NANORTC_STUN_BUF_SIZE must be >= NANORTC_MEDIA_BUF_SIZE when audio or video transport is enabled"
#endif
```

Any override that shrinks the scratch buffer below the media packet size in a
media build becomes an explicit build failure, not a silent runtime packet
drop. This is the guard for the Phase 7 C0 fix: the previous 256 B default
caused every inbound RTP packet above 256 B to return
`NANORTC_ERR_BUFFER_TOO_SMALL`. See the
[Phase 7 exec plan](../exec-plans/completed/phase7-stability-performance-hardening.md)
for the full history.

**Impact**: DC-only builds pay nothing for this scratch — the buffer stays at
256 B. Media builds pay ~976 B more than the pre-Phase-7 default to carry a
single scratch buffer instead of introducing a new field.

## Minimal Embedded Profile

For the most constrained targets (e.g., ESP32 with 320 KB SRAM), use these
overrides in your `NANORTC_CONFIG_FILE` header:

```c
/* Minimal DC-only profile: ~18 KB total */
#define NANORTC_FEATURE_TURN        0
#define NANORTC_MAX_DATACHANNELS     2
#define NANORTC_MAX_ICE_CANDIDATES   4
#define NANORTC_MAX_LOCAL_CANDIDATES 2
#define NANORTC_OUT_QUEUE_SIZE       8
#define NANORTC_SCTP_SEND_BUF_SIZE  2048
#define NANORTC_SCTP_RECV_BUF_SIZE  2048
#define NANORTC_SCTP_MAX_SEND_QUEUE 8
#define NANORTC_SDP_BUF_SIZE        1024
```

## Phase 9 additions (BWE perception)

Already folded into the Configuration Matrix above. Shipping TWCC parsing,
loss-based controller, runtime tunables, and the extended stats fields
costs roughly:

| Addition | Per-instance | Per-video-track | Stack only |
|---|---|---|---|
| `nano_rtp_t.twcc_{ext_id,seq}` | — | +4 B | — |
| `nano_bwe_t` runtime fields (`twcc_count`, `last_source`, `runtime_min/max`, `runtime_event_threshold_pct`) | +16 B | — | — |
| `nanortc_track_t.rate_window` (1 s bucket, prev-second snapshot) | — | +20 B | — |
| `nanortc_track_t.fraction_lost` | — | +4 B (incl. pad) | — |
| TWCC parser per-packet status scratch | — | — | `NANORTC_TWCC_MAX_PACKETS_PER_FB` × 1 B (default 128) |
| RTP wire overhead when TWCC negotiated | — | — | +8 B per outgoing RTP packet on the wire |

Totals for a single-video-track IoT camera: **~44 B** of long-lived RAM and
up to **128 B** on the stack during `twcc_parse_feedback()`. Well inside the
`NANORTC_MEDIA_BUF_SIZE + 32` headroom and the default ESP32 task stack.

To disable TWCC to claw back the 8 B/packet wire overhead (at the cost of
losing bandwidth feedback against modern browsers), set
`NANORTC_TWCC_EXT_ID=0` — the SDP generator will then skip the `a=extmap`
line and `rtp_pack()` will not emit the extension.

## Optimization Techniques Applied

Historical: Phase 6 (2026-04-11) cut full-media RAM by ~34 % (157→103 KB on
the 32-bit ARM reference baseline used at the time). The current defaults
in `nanortc_config.h` reflect that tuning; the matrix above measures
ESP32-P4 after Phase 7 stability hardening and Phase 9 TWCC/BWE additions
folded in on top of the Phase 6 baseline. The six techniques below are
kept for reference:

1. **Config default tuning** (~49 KB saved) — Jitter buffer slots 64→32, slot data 640→320B, H.264 NAL buffer 32→16 KB. All `#ifndef` guarded; override via `NANORTC_CONFIG_FILE`.

2. **Zero-copy CRC-32c** — `nsctp_verify_checksum()` used to copy the entire SCTP packet (1200B) to a stack buffer. Replaced with segmented `nano_crc32c_init/update/final` API that computes CRC in three passes, skipping the checksum field.

3. **Struct field reordering** (~32 B saved) — SCTP `recv_gap` struct reordered to eliminate padding: `uint32_t` fields first, `uint16_t` next, `uint8_t/bool` last (20B→16B per entry × 8).

4. **Type narrowing** (~50 B saved) — Credential length fields (`size_t`→`uint16_t`) in ICE, TURN, and jitter structs. Max credential length is 128 bytes, well within `uint16_t` range.

5. **TURN feature flag** (700B RAM + 13KB code) — `NANORTC_FEATURE_TURN=0` compiles out all TURN relay code. Most embedded deployments are LAN-only and don't need NAT traversal.

6. **Buffer size tightening** — `NANORTC_SDP_FINGERPRINT_SIZE` 128→104 (exact SHA-256 fit), `NANORTC_ICE_REMOTE_PWD_SIZE` 128→48 (covers all browser implementations), `NANORTC_MEDIA_BUF_SIZE` headroom 80→32 bytes.

## Measuring Sizes

```bash
# Regenerate the Configuration Matrix above (ESP32-P4, all six combos).
# Requires a sourced ESP-IDF 5.x environment (IDF_PATH set, riscv32-esp-elf
# toolchain in PATH). Building from a git worktree whose directory is not
# named "nanortc" additionally requires NANORTC_COMPONENT_DIR to point at a
# directory containing a `nanortc` symlink to the worktree root.
./scripts/measure-sizes.sh --esp32 esp32p4

# Host build (macOS/Linux; OpenSSL or mbedtls auto-detected). Numbers are
# slightly larger than ESP32-P4 due to 64-bit pointers.
./scripts/measure-sizes.sh

# sizeof regression tests (CI-integrated, enforces per-struct upper bounds):
cmake -B build -DNANORTC_CRYPTO=openssl
cmake --build build && ctest --test-dir build -R sizeof
```
