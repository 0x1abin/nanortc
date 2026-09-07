# Issue #81: bidirectional ICE checks

Issue: <https://github.com/0x1abin/nanortc/issues/81>

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
when a success response identifies them.

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
  address/port-dependent filtering; both role assignments, srflx trickling,
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

Host `sizeof(nanortc_t)` is unchanged: CORE_ONLY 20,640; DATA 35,760;
AUDIO_ONLY 46,400; AUDIO 61,512; MEDIA_ONLY 102,472; MEDIA 117,592;
MEDIA_H265 118,632 bytes. Host Release archive code/read-only bytes are
97,406 / 135,160 / 123,431 / 160,793 / 135,935 / 173,953 / 181,713 in that order.
These exclude crypto libraries and final-link garbage collection.
No ESP32 hardware, external TURN service or WASI runtime validation is included.
