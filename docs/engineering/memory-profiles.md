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
| SCTP send + recv buffers | ~13 KB | `NANORTC_SCTP_SEND_BUF_SIZE`, `NANORTC_SCTP_RECV_BUF_SIZE` |
| DTLS buffers (3 x 2048) | ~6 KB | `NANORTC_DTLS_BUF_SIZE` (2048) |
| TURN client | ~668 B | `NANORTC_FEATURE_TURN` (disable to save) |

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

## Measuring Sizes

```bash
# Print sizeof(nanortc_t) and sub-structs:
gcc -I include -I src -I crypto \
    -DNANORTC_FEATURE_DATACHANNEL=1 \
    -DNANORTC_FEATURE_AUDIO=0 \
    -DNANORTC_FEATURE_VIDEO=0 \
    tests/test_sizeof.c -o sizeof_test && ./sizeof_test
```
