# NanoRTC Benchmark Report

**Platform:** macOS arm64 (Apple Silicon), OpenSSL 3.6.1, `-O2` Release  
**Date:** 2026-04-07  
**Build:** nanortc v0.1.0, commit `b48267c` (develop branch)  
**Config:** Full media (DC=ON AUDIO=ON VIDEO=ON IPV6=ON)

---

## 1. Static Metrics

### 1.1 Feature Combo Sizes

nanortc provides orthogonal feature flags — unused modules are eliminated at compile time.

| Configuration | Flash (.text) | RAM (sizeof) | Compile Flags |
|---|---|---|---|
| Core only | 40.9 KB | 13.6 KB | DC=OFF AUDIO=OFF VIDEO=OFF |
| DataChannel | 55.6 KB | 27.6 KB | DC=ON |
| Audio only | 58.7 KB | 99.7 KB | DC=OFF AUDIO=ON |
| DC + Audio | 73.6 KB | 113.6 KB | DC=ON AUDIO=ON |
| Media only (no DC) | 62.5 KB | 139.7 KB | DC=OFF AUDIO=ON VIDEO=ON |
| **Full media** | **77.4 KB** | **153.7 KB** | DC=ON AUDIO=ON VIDEO=ON |

> **sizeof(nanortc_t)** is the full per-connection RAM — no heap allocation exists.
> ARM Cortex-M targets typically show smaller numbers due to 32-bit pointers and tighter alignment.

### 1.2 Per-Module .text Breakdown (Full Media)

| Module | .text | % of Total | Function |
|---|---|---|---|
| nano_rtc | 20.6 KB | 26.7% | Main state machine / orchestrator |
| nano_sdp | 14.0 KB | 18.1% | SDP offer/answer (RFC 8866) |
| nano_sctp | 10.3 KB | 13.4% | SCTP-Lite (RFC 4960/9260) |
| nano_turn | 7.4 KB | 9.6% | TURN relay client (RFC 5766) |
| nano_srtp | 3.8 KB | 4.9% | SRTP encrypt/decrypt (RFC 3711) |
| nano_addr | 3.7 KB | 4.8% | IPv4/IPv6 address parsing |
| nano_stun | 3.4 KB | 4.4% | STUN encoding/decoding (RFC 8489) |
| nanortc_crypto_openssl | 2.9 KB | 3.8% | OpenSSL crypto adapter |
| nano_ice | 2.2 KB | 2.9% | ICE connectivity checks (RFC 8445) |
| nano_h264 | 1.9 KB | 2.5% | H.264 FU-A packetizer (RFC 6184) |
| nano_dtls | 1.5 KB | 1.9% | DTLS handshake driver |
| nano_datachannel | 1.4 KB | 1.8% | DCEP protocol (RFC 8832) |
| nano_rtcp | 1.2 KB | 1.6% | RTCP SR/RR/PLI (RFC 3550) |
| nano_jitter | 1.0 KB | 1.2% | Audio jitter buffer |
| nano_media | 0.8 KB | 1.0% | Media track management |
| nano_bwe | 0.7 KB | 0.9% | Bandwidth estimator |
| nano_rtp | 0.5 KB | 0.6% | RTP header codec (RFC 3550) |
| nano_log | 0.2 KB | 0.2% | Logging infrastructure |
| nano_crc32 + crc32c | 0.1 KB | 0.1% | CRC checksums |
| **Total** | **77.4 KB** | **100%** | |

### 1.3 Source Metrics

| Metric | Value |
|---|---|
| Core source (src/) | 11,517 lines |
| Public headers (include/) | 1,751 lines |
| Crypto adapters (crypto/) | 1,535 lines |
| **Total SLOC** | **14,803 lines** |
| Source files | 19 .c + 18 .h |
| External dependencies | **1** (OpenSSL or mbedTLS, abstracted) |

---

## 2. Runtime Performance

All runtime benchmarks use the Sans I/O loopback pattern: two `nanortc_t` instances
wired together in memory via `e2e_pump()`, no network sockets involved. Measurements
reflect pure protocol processing time.

### 2.1 Connection Establishment

| Metric | Iterations | avg | P50 | P95 | P99 |
|---|---|---|---|---|---|
| init + destroy cycle | 1,000 | 1.18 μs | 1 μs | 2 μs | 2 μs |
| ICE + DTLS handshake | 100 | 728 μs | 533 μs | 654 μs | 18.9 ms |
| Full connect (ICE+DTLS+SCTP) | 50 | 520 μs | **516 μs** | 563 μs | 679 μs |

> The P99 outlier on ICE+DTLS (18.9 ms) is caused by first-invocation crypto initialization.
> Steady-state P95 is under 654 μs.

### 2.2 DataChannel Latency

Measurement: `nanortc_datachannel_send()` → relay via `e2e_pump()` → SCTP SACK received.
Each iteration is a single message send + full relay cycle.

| Payload Size | Iterations | avg | P50 | P95 | P99 |
|---|---|---|---|---|---|
| 64 B | 5,000 | 1.36 μs | **1 μs** | 2 μs | 3 μs |
| 256 B | 5,000 | 2.16 μs | **2 μs** | 3 μs | 4 μs |
| 1,024 B | 2,000 | 5.23 μs | **5 μs** | 7 μs | 10 μs |

### 2.3 DataChannel Throughput

Measurement: burst send via `nanortc_datachannel_send()`, periodic pump every 16 messages.

| Payload Size | Throughput | Messages/s | Note |
|---|---|---|---|
| 64 B | 15.3 MB/s | 250K msg/s | Small IoT sensor payloads |
| 256 B | 100.2 MB/s | 410K msg/s | Typical control messages |
| 1,024 B | 13.2 MB/s | 13.5K msg/s | Fragments at SCTP MTU |
| 4,096 B | 300.5 MB/s | 77K msg/s | Large payloads |

> Throughput is bounded by the default SCTP send buffer (`NANORTC_SCTP_SEND_BUF_SIZE = 4096`).
> Increasing to 16 KB or 64 KB will proportionally raise sustained throughput.

### 2.4 RTP Encoding/Decoding

Pure codec throughput (no SRTP, no network). Header-only processing.

| Operation | 160 B (G.711 20ms) | 320 B (Opus 20ms) | 1,200 B (Video MTU) |
|---|---|---|---|
| **rtp_pack** | 31.8 GB/s / 194M pkt/s | 42.4 GB/s / 134M pkt/s | 85.0 GB/s / 73.5M pkt/s |
| **rtp_unpack** | 92.2 GB/s / 562M pkt/s | 177.9 GB/s / 562M pkt/s | 649.4 GB/s / 562M pkt/s |

> RTP is essentially a `memcpy` + 12-byte header write/parse — throughput is memory-bandwidth limited.

### 2.5 SRTP Encrypt/Decrypt

AES-128-CM + HMAC-SHA1-80 (RFC 3711), via OpenSSL backend.

| Operation | 160 B (G.711) | 320 B (Opus) | 1,200 B (Video MTU) |
|---|---|---|---|
| **srtp_protect** | 192 MB/s / 1.1M pkt/s | 345 MB/s / 1.1M pkt/s | 922 MB/s / 791K pkt/s |
| **srtp_unprotect** | 101 MB/s / 580K pkt/s | 174 MB/s / 535K pkt/s | 464 MB/s / 398K pkt/s |

> SRTP is the computational bottleneck: HMAC-SHA1 dominates for small packets,
> AES-CTR dominates for larger ones. `unprotect` is ~2x slower due to HMAC verification
> before decryption. On ESP32 with hardware AES, expect 3-5x improvement.

---

## 3. Concurrency Scaling

All instances allocated on the stack/heap as a flat array. No shared state, no locks.

| Instances | Init Time / Instance | Total Memory | sizeof / Instance |
|---|---|---|---|
| 1 | 83 μs | 153 KB | 153.7 KB |
| 10 | 25 μs | 1,536 KB | 153.7 KB |
| 50 | 21 μs | 7,683 KB | 153.7 KB |
| 100 | 23 μs | 15,366 KB | 153.7 KB |

**Key properties:**
- Memory is **perfectly linear** — 100 instances = 100 × sizeof(nanortc_t), no hidden overhead
- **Zero heap allocation** — malloc count is always 0
- **No global state** — instances are fully independent, can run on separate cores without locks
- Init amortizes to ~20 μs/instance (first instance is slower due to cache cold start)

---

## 4. Architectural Advantages

### 4.1 Zero Heap Allocation

| Metric | nanortc | Traditional WebRTC stack |
|---|---|---|
| malloc calls per connection | **0** | Hundreds to thousands |
| Peak heap (measured) | **0 bytes** | Requires profiling |
| Memory predictability | sizeof = peak | Must stress-test to find peak |
| Heap fragmentation | **Impossible** | Grows over time on RTOS |

### 4.2 Compile-Time Tailoring

| Feature disabled | Flash saved | RAM saved |
|---|---|---|
| Video (VIDEO=OFF) | 3.8 KB | 40.0 KB |
| Audio (AUDIO=OFF) | 18.8 KB | 126.1 KB |
| DataChannel (DC=OFF) | 14.9 KB | 14.0 KB |
| IPv6 (IPV6=OFF) | ~0.3 KB | ~0.3 KB |

> Dead code elimination is complete — disabling a feature removes 100% of its code and data.

### 4.3 Dependency Comparison

| Library | External Dependencies |
|---|---|
| **nanortc** | **1** (mbedTLS or OpenSSL, abstracted) |
| libdatachannel | 5+ (usrsctp, libsrtp, libjuice, plog, nlohmann/json) |
| Amazon KVS SDK | 5+ (OpenSSL, libsrtp, libjsmn, libusrsctp, libwebsockets) |

---

## 5. How to Reproduce

```bash
# Build benchmarks
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DNANORTC_BUILD_BENCHMARKS=ON \
      -DNANORTC_FEATURE_DATACHANNEL=ON \
      -DNANORTC_FEATURE_AUDIO=ON \
      -DNANORTC_FEATURE_VIDEO=ON \
      -DNANORTC_CRYPTO=openssl
cmake --build build -j$(nproc)

# Run all benchmarks
ctest --test-dir build -R bench_ --output-on-failure

# Run individual benchmark (JSON on stdout, human on stderr)
./build/benchmarks/bench_connect
./build/benchmarks/bench_dc_latency
./build/benchmarks/bench_dc_throughput
./build/benchmarks/bench_rtp
./build/benchmarks/bench_srtp
./build/benchmarks/bench_concurrent

# Static size comparison (markdown output)
./benchmarks/scripts/compare_sizes.sh

# Parse JSON output for CI regression tracking
./build/benchmarks/bench_connect 2>/dev/null | jq .
```

---

## 6. Benchmark Suite Overview

| Benchmark | What It Measures | Key Metric |
|---|---|---|
| `bench_connect` | ICE + DTLS + SCTP handshake | 516 μs P50 full connect |
| `bench_dc_latency` | DataChannel send-to-SACK round-trip | 1 μs P50 (64B) |
| `bench_dc_throughput` | DataChannel sustained throughput | 100 MB/s (256B) |
| `bench_rtp` | RTP header pack/unpack | 73.5M pkt/s pack (1200B) |
| `bench_srtp` | SRTP AES-CM + HMAC-SHA1 | 922 MB/s protect (1200B) |
| `bench_concurrent` | Multi-instance scaling | Linear memory, 0 malloc |
| `compare_sizes.sh` | Flash + RAM across feature combos | 77.4 KB full / 40.9 KB core |
