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

## PR Workflow

Each module is one PR. A PR must include:
- Source file (`src/nano_<module>.c`)
- Internal header (`src/nano_<module>.h`)
- Tests (`tests/test_<module>.c`)
- Updated CLAUDE.md if build instructions change

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
