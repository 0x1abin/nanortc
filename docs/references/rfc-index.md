# RFC Reference Index

Authoritative protocol specifications for NanoRTC. RFCs are the **sole** authoritative source for protocol stack implementation — third-party implementations must not be consulted for design or wire-format decisions.

## Core Protocol Stack

| Module | RFC | Title | Key Sections | Priority |
|--------|-----|-------|-------------|----------|
| Mux | [RFC 7983](https://www.rfc-editor.org/rfc/rfc7983) | Multiplexing Scheme Updates for DTLS-SRTP | §3 (demux algorithm by first byte) | Phase 1 |
| STUN | [RFC 8489](https://www.rfc-editor.org/rfc/rfc8489) | Session Traversal Utilities for NAT | §5 (message structure), §6 (attributes), §14 (FINGERPRINT), §15 (MESSAGE-INTEGRITY) | Phase 1 Week 1 |
| STUN Vectors | [RFC 5769](https://www.rfc-editor.org/rfc/rfc5769) | Test Vectors for STUN | §2.1 (request), §2.2 (IPv4 response), §2.3 (IPv6 response) | Phase 1 Week 1 |
| ICE | [RFC 8445](https://www.rfc-editor.org/rfc/rfc8445) | Interactive Connectivity Establishment | §2.2 (ICE Lite), §5.1 (full), §7 (performing checks), §7.3 (responding) | Phase 1 Week 1 |
| TURN | [RFC 5766](https://www.rfc-editor.org/rfc/rfc5766) | Traversal Using Relays around NAT | §6 (Allocate), §7 (Refresh), §9 (CreatePermission), §10 (Send/Data Indication), §11 (Channels), §14 (Attributes) | Phase 5 |
| TURN-bis | [RFC 8656](https://www.rfc-editor.org/rfc/rfc8656) | Traversal Using Relays around NAT (updates 5766) | §6 (Refresh), §7 (Allocate), §9 (CreatePermission), §10 (Send/Data Indication), §11 (Channels), §12 (ChannelData) | Phase 5 |
| TURN IPv6 | [RFC 6156](https://www.rfc-editor.org/rfc/rfc6156) | TURN Extension for IPv6 | §4 (REQUESTED-ADDRESS-FAMILY), §6 (relay address selection) | Future (currently out of scope) |
| DTLS | [RFC 6347](https://www.rfc-editor.org/rfc/rfc6347) | Datagram Transport Layer Security 1.2 | §4 (record protocol), §4.2 (handshake) | Phase 1 Week 2 |
| SCTP | [RFC 4960](https://www.rfc-editor.org/rfc/rfc4960) | Stream Control Transmission Protocol | §3 (packet format), §5 (association setup), §6 (chunk processing), §8 (fault) | Phase 1 Week 3 |
| SCTP/DTLS | [RFC 8261](https://www.rfc-editor.org/rfc/rfc8261) | Datagram Transport Layer Security for SCTP | Entire document | Phase 1 Week 3 |
| PR-SCTP | [RFC 3758](https://www.rfc-editor.org/rfc/rfc3758) | SCTP Partial Reliability Extension | §3 (FORWARD-TSN) | Phase 1 Week 3 |
| DC Transport | [RFC 8831](https://www.rfc-editor.org/rfc/rfc8831) | WebRTC Data Channels | Entire document | Phase 1 Week 4 |
| DCEP | [RFC 8832](https://www.rfc-editor.org/rfc/rfc8832) | WebRTC Data Channel Establishment Protocol | §5 (DATA_CHANNEL_OPEN), §6 (DATA_CHANNEL_ACK) | Phase 1 Week 4 |
| SDP | [RFC 8866](https://www.rfc-editor.org/rfc/rfc8866) | Session Description Protocol | §5 (SDP spec), §9 (attributes) | Phase 1 Week 4 |
| JSEP | [RFC 8829](https://www.rfc-editor.org/rfc/rfc8829) | JavaScript Session Establishment Protocol | §5 (SDP offer/answer) | Phase 1 Week 4 |

## Media (Phase 2-3)

| Module | RFC | Title | Key Sections | Priority |
|--------|-----|-------|-------------|----------|
| RTP | [RFC 3550](https://www.rfc-editor.org/rfc/rfc3550) | RTP: A Transport Protocol for Real-Time Applications | §5 (RTP header), §6 (RTCP) | Phase 2 Week 1 |
| RTP Profiles | [RFC 3551](https://www.rfc-editor.org/rfc/rfc3551) | RTP Profile for Audio and Video | §4.5 (G.711), §6 (payload types) | Phase 2 Week 1 |
| SRTP | [RFC 3711](https://www.rfc-editor.org/rfc/rfc3711) | The Secure Real-time Transport Protocol | §3 (SRTP framework), §4 (transforms) | Phase 2 Week 1 |
| DTLS-SRTP | [RFC 5764](https://www.rfc-editor.org/rfc/rfc5764) | DTLS Extension to Establish Keys for SRTP | §4 (key material export) | Phase 2 Week 1 |
| RTCP FB | [RFC 4585](https://www.rfc-editor.org/rfc/rfc4585) | Extended RTP Profile for RTCP-Based Feedback | §6.2.1 (NACK), §6.3.1 (PLI) | Phase 2 Week 2 |
| Opus RTP | [RFC 7587](https://www.rfc-editor.org/rfc/rfc7587) | RTP Payload Format for Opus | Entire document | Phase 2 Week 1 |
| H.264 RTP | [RFC 6184](https://www.rfc-editor.org/rfc/rfc6184) | RTP Payload Format for H.264 Video | §5.8 (FU-A fragmentation) | Phase 3 Week 1 |
| VP8 RTP | [RFC 7741](https://www.rfc-editor.org/rfc/rfc7741) | RTP Payload Format for VP8 Video | §4 (payload format) | Phase 3 Week 1 (optional) |
| H.265 RTP | [RFC 7798](https://www.rfc-editor.org/rfc/rfc7798) | RTP Payload Format for HEVC | §1.1.4 (NAL header), §4.4.1 (Single NAL), §4.4.2 (Aggregation Packet), §4.4.3 (Fragmentation Unit), §7.1 (SDP parameters) | Phase 3.5 |
| Base64 | [RFC 4648](https://www.rfc-editor.org/rfc/rfc4648) | The Base16, Base32, and Base64 Data Encodings | §4 (base64 alphabet), §10 (test vectors) | Phase 3.5 (sprop-vps/sps/pps fmtp) |

## Security

| RFC | Title | Relevance |
|-----|-------|-----------|
| [RFC 8827](https://www.rfc-editor.org/rfc/rfc8827) | WebRTC Security Architecture | Mandatory SRTP cipher suites, DTLS requirements |
| [RFC 8826](https://www.rfc-editor.org/rfc/rfc8826) | Security Considerations for WebRTC | Threat model and mitigations |

## How to Use This Index

1. **Before implementing a module**, read the corresponding RFC sections listed above
2. **When writing parser code**, cite the RFC section in a comment: `/* RFC 8489 §5.1 */`
3. **When a behavior is ambiguous**, check the RFC's MUST/SHOULD/MAY language
4. **Test vectors**: Check RFC appendices first; many include example packets
