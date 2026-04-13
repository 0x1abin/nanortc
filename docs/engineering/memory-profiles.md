# Memory Profiles

Approximate `sizeof(nanortc_t)` for various feature configurations on a 32-bit ARM target.
Host (64-bit) sizes are slightly larger due to pointer/size_t widths.

## Configuration Matrix

| Configuration | nanortc_t (approx) | Notes |
|---|---|---|
| `CORE_ONLY` (no DC, no media, TURN=ON) | ~14 KB | ICE + DTLS + SDP + TURN |
| `CORE_ONLY` (no DC, no media, TURN=OFF) | ~13 KB | Minimal WebRTC core |
| `DC-only` (TURN=ON) | ~28 KB | DataChannel: adds SCTP + DCEP |
| `DC-only` (TURN=OFF) | ~27 KB | Typical LAN-only IoT config |
| `DC + Audio` | ~39 KB | Adds 1 audio track (~11 KB jitter buffer) |
| `DC + Audio + Video` | ~103 KB | Adds 1 video track + pkt_ring + BWE |

## Biggest Contributors

| Component | Default Size | Tuning Knob |
|---|---|---|
| Jitter buffer (per audio track) | ~11 KB | `NANORTC_JITTER_SLOTS` (32), `NANORTC_JITTER_SLOT_DATA_SIZE` (320) |
| H.264 NAL reassembly (per video track) | ~16 KB | `NANORTC_VIDEO_NAL_BUF_SIZE` (16384) |
| Video packet ring (32 slots) | ~39 KB | `NANORTC_OUT_QUEUE_SIZE` (32), `NANORTC_MEDIA_BUF_SIZE` |
| SCTP send + recv + gap buffers | ~13 KB | `NANORTC_SCTP_SEND_BUF_SIZE`, `NANORTC_SCTP_RECV_BUF_SIZE`, `NANORTC_SCTP_RECV_GAP_BUF_SIZE` |
| DTLS buffers (3 x 2048) | ~6 KB | `NANORTC_DTLS_BUF_SIZE` (2048) |
| Shared STUN/RTCP/RTP scratch | 256 B (DC-only) / ~1232 B (media) | `NANORTC_STUN_BUF_SIZE` (feature-gated — see below) |
| TURN client | ~668 B | `NANORTC_FEATURE_TURN` (disable to save) |

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

## Optimization Techniques Applied

The following techniques were used to reduce RAM by 34% (full-media: 157→103 KB):

1. **Config default tuning** (~49 KB saved) — Jitter buffer slots 64→32, slot data 640→320B, H.264 NAL buffer 32→16 KB. All `#ifndef` guarded; override via `NANORTC_CONFIG_FILE`.

2. **Zero-copy CRC-32c** — `nsctp_verify_checksum()` used to copy the entire SCTP packet (1200B) to a stack buffer. Replaced with segmented `nano_crc32c_init/update/final` API that computes CRC in three passes, skipping the checksum field.

3. **Struct field reordering** (~32 B saved) — SCTP `recv_gap` struct reordered to eliminate padding: `uint32_t` fields first, `uint16_t` next, `uint8_t/bool` last (20B→16B per entry × 8).

4. **Type narrowing** (~50 B saved) — Credential length fields (`size_t`→`uint16_t`) in ICE, TURN, and jitter structs. Max credential length is 128 bytes, well within `uint16_t` range.

5. **TURN feature flag** (700B RAM + 13KB code) — `NANORTC_FEATURE_TURN=0` compiles out all TURN relay code. Most embedded deployments are LAN-only and don't need NAT traversal.

6. **Buffer size tightening** — `NANORTC_SDP_FINGERPRINT_SIZE` 128→104 (exact SHA-256 fit), `NANORTC_ICE_REMOTE_PWD_SIZE` 128→48 (covers all browser implementations), `NANORTC_MEDIA_BUF_SIZE` headroom 80→32 bytes.

## Measuring Sizes

```bash
# Run sizeof regression tests (CI-integrated):
cmake -B build -DNANORTC_CRYPTO=openssl
cmake --build build && ctest --test-dir build -R sizeof

# Print sizeof(nanortc_t) and sub-structs for a specific config:
gcc -I include -I src -I crypto \
    -DNANORTC_FEATURE_DATACHANNEL=1 \
    -DNANORTC_FEATURE_AUDIO=0 \
    -DNANORTC_FEATURE_VIDEO=0 \
    tests/test_sizeof.c -o sizeof_test && ./sizeof_test
```
