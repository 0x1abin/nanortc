# Issue #81: ICE checks and candidate signaling

Issue: <https://github.com/0x1abin/nanortc/issues/81>

## Firefox candidate serialization follow-up (2026-09-11)

The reporter confirmed NAT traversal with Chromium after #83/#84, then isolated
Firefox's remaining failure to missing `raddr`/`rport` in trickled srflx candidates.
The shared trickle formatter also omitted these fields for relay candidates.
Full SDP omitted them without a host candidate and otherwise used the first host
address, which was not necessarily the srflx base or the TURN mapped address.

Both output paths now consistently hide related addresses according to
[RFC 8839 §5.1 and §9.1](https://www.rfc-editor.org/rfc/rfc8839.html#section-5.1):

| Candidate | Related fields |
|-----------|----------------|
| host | Omitted |
| IPv4 srflx / relay | `raddr 0.0.0.0 rport 9` |
| IPv6 srflx / relay | `raddr :: rport 9` |

The candidate's own address selects the family, independently of host candidates.
This deliberately removes real related-address diagnostics from SDP; it does not
hide separately advertised host candidates or alter the socket/base used by ICE.
The reporter's `rport 0` workaround is replaced by the RFC's discard port `9`.
Public APIs, structure sizes, candidate scratch capacity and output lifetimes
are unchanged. No related-address state, configuration or allocation was added.

Validation of this follow-up:

- New RFC-derived output assertions failed on the old formatter and pass with
  the fix. Tests inspect complete candidate tails in events and individual SDP
  lines, including no-host and mixed-family cases. Maximum string arguments,
  NUL termination, canaries and exact/one-byte-short SDP buffers are covered.
- STUN discovery and TURN allocation E2E tests assert the actual event suffixes.
  The existing shared STUN/TURN test now has its missing TURN feature guard so
  TURN-disabled test builds compile.
- Full local CI: **54/54** checks, including seven feature combinations with
  OpenSSL and mbedTLS and local interoperability tests. Combined ASan/UBSan:
  **34/34** suites. DATA with IPv6, TURN and ICE_SRFLX all disabled: **17/17**.
- Firefox **155.0.1** accepted eight cases from actual library-generated output:
  IPv4/IPv6 × srflx/relay × `addIceCandidate`/full SDP. Hidden address and port
  values were also checked through `RTCIceCandidate`.
- Firefox **155.0.1** and Chrome **147.0.7727.137**, using the OpenSSL browser
  example, passed both roles with **18** DataChannel echoes per role, including
  empty, 4096-byte, UTF-8 and embedded-NUL messages. Firefox reused the existing
  browser exercise through a temporary localhost WebDriver harness; its binary
  type check used an ArrayBuffer tag because WebDriver objects cross JS realms.

These browser connections used local host candidates. Candidate parsing with
documentation addresses does not validate their reachability. Real cross-NAT,
external TURN, IPv6 connectivity and WASI runtime testing are not included in
this follow-up. The earlier ICE implementation and validation are recorded below.

## Problem and behavior

Before this change, only the controlling agent initiated checks. The controlled
agent responded to requests and immediately accepted USE-CANDIDATE. Discovering
a STUN mapping therefore did not cause the controlled endpoint to send to the
peer and open a NAT's address/port-dependent inbound filter. This is a confirmed
implementation limitation, not a confirmed diagnosis of the reporter's WASI setup.

Both roles now check connectivity. A single const task reader selects ordinary,
triggered, retransmission and expiry work and supplies the next deadline. RTC
provides TURN permission readiness and acquires a transmit slot before committing
a task; ICE returns the actual candidate indices. Waiting for credentials,
candidates or permissions no longer produces a zero-deadline spin.

The controlling agent nominates a validated pair with a separate transaction.
The controlled agent retains an incoming nomination until its reverse check
succeeds. DTLS starts only after these conditions are satisfied. STUN PRIORITY
uses the prospective peer-reflexive preference (RFC 8445 §7.1.1); SDP host,
srflx and relay priorities retain their own type preferences.

Authenticated incoming checks can register a remote peer-reflexive candidate
and queue a reverse check. The fixed pending table retains valid pairs and
triggered work in the former padding byte. Full tables never evict live
transactions. An incoming nomination that cannot be retained is not acknowledged;
its sender can retransmit after capacity becomes available. Duplicate incoming
checks do not continually reset the triggered transaction. Invalid responses
and responses for cancelled transactions do not change its state.

Retransmission preserves the transaction ID and request content, uses an initial
`NANORTC_ICE_RTO_MS` of 500 ms and exponential backoff, and expires at
`NANORTC_ICE_CHECK_TIMEOUT_MS`. `NANORTC_ICE_MAX_CHECKS` bounds new checks;
nomination of a validated pair can finish after that budget is reached.

The STUN discovery base is retained separately from the mapped address. Checks
and selected-path traffic use the host socket as their source hint, including
when the selected local candidate is srflx. Known mapped candidates are reused
when a success response identifies them. If discovery used the default socket
before any host candidate was registered, source hints remain unset so the
application continues using that socket instead of binding the public mapping.

## Compatibility and limits

Public function signatures are unchanged. Internal ICE state and task interfaces
changed: rebuild the application and library together. Structure ceilings are
unchanged; no packet cache, heap allocation or candidate-pair matrix was added.

This remains a bounded UDP ICE subset: a complete priority-sorted checklist,
foundation unfreezing, automatic 487 role repair and additional local
peer-reflexive mappings are not implemented. STUN alone cannot guarantee traversal
of every NAT; endpoint-dependent mapping or disabled hairpinning can still
require TURN or a different reachable candidate.

## Validation

- `test_ice_nat`: two private endpoints with separate mappings and
  address/port-dependent filtering; both role assignments, with and without registered host candidates, srflx trickling,
  ICE/DTLS/SCTP/DCEP and bidirectional UTF-8 text containing an embedded NUL.
  The same test fails against the pre-change main revision. A bounded test
  packet queue retains input across output backpressure, including one TX slot.
- ICE regressions cover controlled checks, regular and delayed nomination,
  full tables, authenticated admission, retired responses, duplicates, late
  credentials, same-family selection, random-provider rollback and clock wrap.
- Existing local fake-STUN interop remains a candidate-routing test; it is not
  evidence of traversal through a real NAT.

Final native validation (2026-09-07): full `scripts/ci-check.sh` **54/54**;
combined ASan/UBSan **32/32** suites; Chrome **147.0.7727.137**, OpenSSL and
mbedTLS, both roles, **18** echo cases per session (including 4096-byte, empty,
UTF-8 and embedded-NUL messages), with Opus reception and decoded H.264 frames.
The browser runner exposed a shutdown/profile-directory cleanup race; bounded
filesystem retries are delivered in the separate example-hardening change.

The combined tree with [example hardening, PR #84](https://github.com/0x1abin/nanortc/pull/84)
also passed **34/34** ASan/UBSan suites and both Chrome roles with both crypto
backends, including candidate forwarding and audio/video. The OpenSSL build
explicitly disabled `getifaddrs`. The temporary worktree initially lacked the
media sample submodule; browser validation passed after using the existing
pinned samples. Final CI reused the local v0.22.5 interop dependency after its
network download failed.

Host `sizeof(nanortc_t)` is unchanged: CORE_ONLY 20,640; DATA 35,760;
AUDIO_ONLY 46,400; AUDIO 61,512; MEDIA_ONLY 102,472; MEDIA 117,592;
MEDIA_H265 118,632 bytes. Host Release archive code/read-only bytes are
97,838 / 135,560 / 123,863 / 161,193 / 136,367 / 174,353 / 182,113 in that order.
These exclude crypto libraries and final-link garbage collection.
No ESP32 hardware, external TURN service or WASI runtime validation is included.
