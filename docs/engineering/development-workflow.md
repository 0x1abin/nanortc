# Development Workflow

## Module Implementation Order

Follow the protocol dependency chain — each module depends on the ones above it:

1. **STUN codec** (`nano_stun.c`) — foundation, no internal deps
2. **ICE** (`nano_ice.c`) — depends on STUN; controlled (answerer) + controlling (offerer) roles
3. **CRC-32c** (`nano_crc32c.c`) — self-contained utility
4. **DTLS** (`nano_dtls.c`) — depends on crypto provider
5. **SCTP-Lite** (`nano_sctp.c`) — depends on DTLS, CRC-32c
6. **DataChannel** (`nano_datachannel.c`) — depends on SCTP
7. **SDP** (`nano_sdp.c`) — integrates with all above

Audio/Media modules (Phase 2-3):

8. **SRTP** (`nano_srtp.c`) — depends on crypto provider
9. **RTP** (`nano_rtp.c`) — standalone codec
10. **RTCP** (`nano_rtcp.c`) — depends on RTP
11. **Jitter buffer** (`nano_jitter.c`) — depends on RTP
12. **BWE** (`nano_bwe.c`) — depends on RTCP (MEDIA only)

## Per-Module Workflow

1. **Read the RFC** — identify the specific sections covering packet format, state machine, and required behavior
2. **Write test vectors** — create `test_<module>.c` with known-good byte sequences (from RFC appendix, pcap, or browser captures)
3. **Implement the codec** — parser and encoder for wire format
4. **Implement the state machine** — transitions and side effects
5. **Run tests** — `cd build && make && ctest --output-on-failure`
6. **Format** — `clang-format -i src/nano_<module>.c src/nano_<module>.h`
7. **Verify constraints** — no forbidden includes, no malloc, profile guards present

## RFC Testing Iron Rule

**Any implementation related to an RFC standard MUST have tests independently generated from the RFC document itself.** Roundtrip tests (encode → parse our own output) are supplementary — they CANNOT serve as primary verification because they cannot detect encoder and parser sharing the same bug.

### Required test categories for every RFC-based module

1. **RFC test vectors (mandatory)** — Hardcode the exact byte sequences from the RFC's test vector document (e.g., RFC 5769 for STUN, RFC 4960 §A for SCTP) as `static const uint8_t[]` arrays. Parse them and verify every field against the RFC's expected values.

2. **External implementation captures (mandatory)** — Use real packet data from browser/wireshark captures. This catches encoding quirks that RFC vectors may not cover (e.g., attribute ordering, padding style, unknown extensions). Do not source captures from `.local-reference/` third-party source trees.

3. **Integrity / checksum verification (mandatory if applicable)** — Verify MESSAGE-INTEGRITY (HMAC), FINGERPRINT (CRC), checksums against the RFC's known-good values using the documented password/key. Test with both correct and incorrect keys.

4. **Roundtrip tests (supplementary)** — Encode with our encoder, parse back, verify all fields match. These test our encoder's correctness but cannot validate interoperability.

5. **Edge cases from RFC text (mandatory)** — Each "MUST" / "MUST NOT" / "SHOULD" in the RFC becomes a test case. Organize tests by RFC section number in comments.

6. **Interop testing against libdatachannel (mandatory for Phase 1+ acceptance)** — Every protocol module that participates in the full connection lifecycle (ICE, DTLS, SCTP, DataChannel, SDP) must pass end-to-end interop tests against libdatachannel. These tests validate that nanortc can establish a real WebRTC connection and exchange DataChannel messages with a known-good third-party implementation over localhost UDP. See `tests/interop/` for the framework.

### Test vector source index

| Module | RFC Test Vectors | Reference Captures | Interop |
|--------|------------------|--------------------|---------|
| STUN | RFC 5769 §2.1-2.3 (Request, IPv4/IPv6 Response) | browser pcap | via ICE interop |
| ICE | — | — | `test_interop_handshake` (libdatachannel) |
| SCTP | RFC 4960 §A (INIT, INIT-ACK, DATA, SACK examples) | browser pcap | `test_interop_handshake` (libdatachannel) |
| DTLS | — (use OpenSSL s_client captures) | browser DTLS handshake | `test_interop_handshake` (libdatachannel) |
| DataChannel | — | browser DCEP OPEN/ACK captures | `test_interop_dc_*` (libdatachannel) |
| SDP | — (use Chrome/Firefox offer strings) | browser SDP offers | `test_interop_handshake` (libdatachannel) |
| RTP | RFC 3550 §A.1 (SR/RR examples) | browser RTP captures | — |
| SRTP | RFC 3711 §B (test vectors) | — | — |

## Interop Testing Workflow

The interop test framework (`tests/interop/`) validates nanortc against libdatachannel over real localhost UDP sockets. This is a **mandatory acceptance gate** for Phase 1 completion and all subsequent phases.

### Architecture

- **Single process, dual-threaded**: nanortc runs as answerer (CONTROLLED) with a select()-based event loop; libdatachannel runs as offerer (CONTROLLING) with its internal thread pool
- **Signaling**: SDP offer/answer and ICE candidates exchanged via a socketpair pipe
- **Transport**: Real UDP on 127.0.0.1 — exercises the full protocol stack (ICE → DTLS → SCTP → DCEP)

### Running interop tests

```bash
# Build (requires OpenSSL + C++ compiler for libdatachannel)
cmake -B build -DNANORTC_CRYPTO=openssl \
      -DNANORTC_BUILD_INTEROP_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)

# Run interop tests only
ctest --test-dir build -R interop --output-on-failure

# Run all tests (unit + interop)
ctest --test-dir build --output-on-failure
```

### When to add interop tests

Add a new interop test case when:
- A protocol module gains enough functionality to participate in a real connection
- A new DataChannel feature is implemented (e.g., multiple channels, large messages)
- A bug is found via browser testing that the unit tests did not catch

### Interop test files

| File | Purpose |
|------|---------|
| `interop_common.{h,c}` | Signaling pipe (socketpair) + timing utilities |
| `interop_nanortc_peer.{h,c}` | nanortc thread wrapper (reuses `run_loop_linux.c`) |
| `interop_libdatachannel_peer.{h,c}` | libdatachannel C API wrapper (callback-based) |
| `test_interop_dc.c` | DataChannel interop test cases |

## PR Workflow

Each module is one PR. A PR must include:
- Source file (`src/nano_<module>.c`)
- Internal header (`src/nano_<module>.h`)
- Tests (`tests/test_<module>.c`)
- Updated CLAUDE.md if build instructions change

## Local CI loop

`scripts/ci-check.sh` mirrors GitHub Actions but is tuned to run fast on a
developer machine:

- **`./scripts/ci-check.sh --fast`** — pre-push tier. Runs arch checks +
  clang-format + DATA build + MEDIA build + ASan, skips the AUDIO_ONLY /
  MEDIA_ONLY / CORE_ONLY combos, mbedtls combos, and the libdatachannel
  interop suite. Cold build ≈ 40 s; warm (incremental) build ≈ 5 s.
- **`./scripts/ci-check.sh`** — full matrix. Mirrors GitHub Actions
  exactly: 6 feature combos × 2 crypto backends + ASan + libdatachannel
  interop. Run before pushing to `main` or release branches.
- **`./scripts/ci-check.sh --clean`** — wipe `.cache/ci/build-ci-*` first.
  Use after a flag or toolchain change that may have left stale state.

Two speedups make this work and are auto-detected:

- **ccache** — install via `brew install ccache`. The script wires it in
  as `CMAKE_C_COMPILER_LAUNCHER`; on a warm cache, identical TUs compile
  in microseconds. Inspect with `ccache -s`.
- **Persistent build dirs** — `.cache/ci/build-ci-*` are kept across
  runs. cmake reuses `CMakeCache.txt` and the ninja/make incremental
  graph, so unchanged TUs are skipped at the build-system level too.

The full matrix without these speedups runs in 8-10 minutes; with them, a
warm `--fast` run is under 10 seconds.

## RFC Reference by Module

| Module | RFC | Key Sections |
|--------|-----|-------------|
| STUN | RFC 8489 | 3 (overview), 5 (STUN message structure), 6 (attributes), 14 (FINGERPRINT), 15 (MESSAGE-INTEGRITY) |
| ICE | RFC 8445 | 2.2 (lite), 5.1 (full), 7 (performing checks), 7.3 (responding to checks) |
| DTLS | RFC 6347 | 4 (record protocol), 4.2 (handshake) |
| SCTP | RFC 4960 | 3 (packet format), 5 (association), 6 (chunk processing), 8 (fault management) |
| SCTP-over-DTLS | RFC 8261 | entire document |
| PR-SCTP | RFC 3758 | 3 (FORWARD-TSN) |
| DataChannel | RFC 8831 + 8832 | 8831: transport. 8832: DCEP messages |
| RTP | RFC 3550 | 5 (RTP data transfer), 6 (RTP control protocol) |
| SRTP | RFC 3711 | 3 (SRTP framework), 4 (pre-defined transforms) |
| SDP | RFC 8866 | 5 (SDP specification), 9 (SDP attributes) |
| Mux | RFC 7983 | 3 (demultiplexing algorithm) |
