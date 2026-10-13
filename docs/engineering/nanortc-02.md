# NanoRTC 0.2.0 migration and acceptance record

0.2.0 changes the public API and connection layout. Rebuild the library and all
callers together. The implementation targets bounded embedded audio/video
sessions; it does not claim complete WebRTC, ICE, SCTP, or browser coverage.
The declarations in [nanortc.h](../../include/nanortc.h) define the exact API.

## Migration checklist

| 0.1 integration | 0.2.0 integration |
|---|---|
| NUL-terminated remote SDP/candidate input | Pass the actual byte length, excluding any terminator. Embedded NUL is rejected. |
| Poll a transmit, then forget it | Complete each transmit token with `nanortc_report_transmit`. |
| Retain a borrowed transmit across another call | Copy the one pending datagram before any state-changing call. |
| Assume `send_*` or polling means UDP sent | Treat admission, dequeue, socket acceptance, and peer delivery separately. |
| Call `accept_answer` on a fresh connection | Call `create_offer` first; only one local offer may be pending. |
| Change track direction during a session | Configure before negotiation and check the returned error. General renegotiation is unsupported. |
| Close one DataChannel | `nanortc_datachannel_close` returns `NANORTC_ERR_NOT_IMPLEMENTED`; close the connection when required. |
| Wait for graceful disconnect completion | `nanortc_disconnect` closes immediately and is idempotent. |
| Read internal fields for diagnostics | Use `nanortc_get_stats` and `nanortc_get_track_stats`. |
| Implement a custom crypto provider | Add required `dtls_get_peer_fingerprint`, returning the peer certificate's 32-byte SHA-256 digest. |
| Rely on four default in-flight FEC buffers | The default `NANORTC_FEC_TX_RING` is 2; override explicitly when the memory budget allows. |

The obsolete `NANORTC_EV_DATACHANNEL_BUFFERED_LOW` event has been removed. An internal
queue becoming writable is not a network transmission acknowledgement.

## Length-delimited negotiation

```c
int nanortc_accept_offer(nanortc_t *rtc, const char *offer, size_t offer_len,
                         char *answer, size_t capacity, size_t *out_len);
int nanortc_create_offer(nanortc_t *rtc, char *offer, size_t capacity,
                         size_t *out_len);
int nanortc_accept_answer(nanortc_t *rtc, const char *answer, size_t answer_len);
int nanortc_add_remote_candidate(nanortc_t *rtc, const char *candidate, size_t len);
int nanortc_set_direction(nanortc_t *rtc, uint8_t mid, nanortc_direction_t direction);
```

`READY → HAVE_LOCAL_OFFER → NEGOTIATED` describes the offerer. The answerer goes
from `READY` to `NEGOTIATED` after accepting an offer. Signaling state is separate
from ICE/DTLS/SCTP connection progress. Repeated local offers, answers without a
pending offer, and glare while a local offer is pending return `ERR_STATE`.
There is no implicit rollback operation or arbitrary track renegotiation.

Remote SDP is parsed into a bounded remote-only result. ICE credentials,
candidates, media parameters and fingerprint are validated before commit;
short output buffers, malformed SDP and unsupported negotiation preserve the
live connection. Output buffers may contain partial or preliminary bytes after
an error and must not be sent. Full SDP and trickled candidates use the same
bounded parser. Component 1 over UDP and numeric IPv4/IPv6 addresses are
supported. Candidate priority and related address/port are retained. Non-host
candidates require a complete `raddr`/`rport` pair; host candidates omit them.
Malformed addresses, duplicate related fields and ports above 65535 are rejected.
The committed related metadata is in `remote_candidate_related`, indexed with
`ice.remote_candidates`; a duplicate updates type, priority and related fields
together. ICE restart clears it. A full RFC 8445 checklist sort remains outside
this implementation's bounded ICE subset.

The supported authentication identity is a SHA-256 certificate fingerprint.
Missing or malformed fingerprints fail negotiation. At DTLS handshake completion,
the provider's actual peer certificate digest must match the SDP digest before
`ESTABLISHED`, SRTP key export, or application data. A mismatch returns
`ERR_CRYPTO`. OpenSSL reads the peer certificate; mbedTLS caches the leaf digest
in its verification callback, including builds that do not retain the peer
certificate after the handshake. See [RFC 8122 §5.1](https://www.rfc-editor.org/rfc/rfc8122.html#section-5.1).

## Output ownership and socket completion

```c
int nanortc_report_transmit(nanortc_t *rtc, uint64_t token,
                            nanortc_transmit_result_t result, uint32_t at_ms);
```

There is at most **one outstanding transmit token per connection**. Polling
returns borrowed payload bytes plus source/destination metadata and a token.
Consume or copy them before another state-changing call on that instance,
including polling, input, sends, reporting completion, restart or disconnect.
A token owns only a completion obligation; it does not extend payload lifetime.
This includes packets lazily wrapped into shared TURN scratch.

1. If UDP accepts the complete datagram, report `NANORTC_TRANSMIT_SENT` with the
   time of socket acceptance.
2. On temporary `EAGAIN`, copy the datagram and routing metadata into one
   application-owned slot, retain its token, and retry when writable. Continue
   receive processing and timer ticks. Do not report a temporary failure as sent.
3. On final discard, report `NANORTC_TRANSMIT_DROPPED` with the discard time.
   The next transmit can then be polled.

Polling cannot dequeue another transmit until completion (`ERR_WOULD_BLOCK`);
queued events can still be delivered. The caller must not poll in an unbounded
spin loop. Input and timer progression remain available.
`at_ms` uses the same monotonic millisecond clock as `handle_input`; reporting
completion does not advance protocol timers. Poll time is not send time.
`SENT` records local socket acceptance, not peer acknowledgement or delivery.

Unknown, duplicate, stale or invalid-result reports return `ERR_INVALID_PARAM`
without changing the pending result. Disconnect invalidates an outstanding
completion; a later report fails. Applications own and release their copied
retry buffer independently. See the shared [example UDP helper](../../examples/common/udp_send.h)
and [Linux event loop](../../examples/common/run_loop_linux.c) for integration.

## Backpressure and lifecycle

`ERR_WOULD_BLOCK` means a temporary resource limit: retain the uncommitted
application frame/message and retry after draining or completing pending output.
`ERR_BUFFER_TOO_SMALL` means the object cannot fit the configured capacity or the
supplied answer buffer; polling cannot repair that permanent size mismatch.
`output_free_slots` covers the universal queue only; protocol, packet-history,
pacer and transient TX pools have independent bounds.

DataChannel message admission and video-frame packet admission reserve their
required bounded storage before commit. Receive DataChannel messages remain in
the SCTP receive pool until polled. Scalar events that encounter a full output
queue retain a latest snapshot by event type and track/channel; this is bounded
coalescing, not an unlimited event log. Realtime media follows its existing
playout and drop policy.

`nanortc_disconnect` immediately enters `CLOSED`, discards queued protocol work,
invalidates the pending transmit and retains the disconnection notification.
It does not perform SCTP SHUTDOWN/ACK/COMPLETE or wait for DTLS close-notify.
`nanortc_destroy` releases provider resources after use. Individual DataChannel
closure remains unsupported until a complete stream-reset exchange is available.

## ICE restart

`nanortc_ice_restart` refreshes ICE credentials and clears remote candidate/check
state. It returns `ERR_WOULD_BLOCK` while a transmit is awaiting completion.
An in-progress DTLS handshake returns `ERR_STATE`; restart is not used to move
a partially completed association between ICE generations.
A live connection preserves its DTLS certificate, authenticated association,
SRTP state, media tracks and DataChannels. The new offer/answer must preserve
peer fingerprint, negotiated codecs, tracks and DTLS role; identity or media
changes are rejected. The restart flag remains set until the new ICE pair is
selected, when transport routing is updated. An incoming valid ICE restart can
also be answered on an already negotiated connection. During checks, the old
selected route remains usable only until its existing consent expires.

This follows [RFC 8842 §6](https://www.rfc-editor.org/rfc/rfc8842.html#section-6):
an ICE restart does not by itself require a new DTLS association. Replacing the
peer or changing tracks requires a new connection in this release.

## Statistics and congestion feedback

`nanortc_get_stats` and `nanortc_get_track_stats` are pure snapshots under the
usual single-owner discipline. Reading them never rolls the live rate counters.
Connection `packets_sent`/`bytes_sent` advance on `SENT`, including outer TURN
UDP bytes when relayed; `packets_dropped` counts reported final discards. Queue,
wrap, packet-ring and feedback-history counters describe their own failure
points and are not interchangeable with network loss.

Track RTP packets/payload octets and outgoing bitrate advance on successful
transmit reports. `packets_accepted` and `frames_accepted` count application
admission before socket completion. Video FPS counts admitted frames, so it is
not proof that a complete frame arrived at the peer. `fraction_lost` comes from
peer RTCP reports.
RTT is available only when a report's SSRC/LSR matches a sent Sender Report;
`rtt_valid=false` means unavailable. DLSR is the peer's waiting time and is
subtracted from elapsed time; it is not itself RTT.

REMB is a receiver cap and can override the encoder's configured minimum. TWCC
feedback uses the **draft-holmer transport-wide-cc format**, RTCP PT=205/FMT=15;
this is not the RFC 8888 congestion-control feedback wire format. A bounded send
history is populated only after actual socket acceptance. Feedback without a
matching send record is unknown, rather than evidence of loss or spare capacity.
Duplicates, retransmit ambiguity, stale feedback, low offered load, recent local
drops and growing arrival delay prevent unsupported upward probes. Summary-only
loss feedback may lower the target but cannot prove spare capacity.

The control interval and stable/load/delay guards are configurable in
`nanortc_config.h`. This is a conservative embedded controller, not a complete
GCC trendline/probe-cluster implementation. It has no receiver-side TWCC sender.
History saturation is observable and limits usable feedback coverage. The pacer
and optional capability ladder consume the estimate; applications still own
encoder reconfiguration. See [adaptive-media.md](adaptive-media.md).

## Resource and validation record

SDP candidates store binary addresses, family, port, type, priority and related
metadata; text is formatted only for SDP/candidate output. Parsed remote
candidates are staged temporarily and committed to ICE without a persistent
duplicate SDP array. Remote fingerprint storage is 32 bytes.
Negotiation staging excludes persistent H.265 parameter sets. A host debug
`-fstack-usage` build bounds the library negotiation/rendering chain below 3 KiB;
this is not an ESP crypto-stack high-water measurement. Crypto allocations,
application retry buffers and RTOS stacks are outside `sizeof(nanortc_t)`.

The default FEC transmit ring changes from four to two buffers to keep the
existing all-advanced profile ceiling. With pacing enabled, groups beyond the
available repair slots are discarded. Without pacing, frame admission reserves
the entire media frame first: if the remaining output/FEC capacity cannot fit
all repair groups completed by that frame, all of those repair groups are
discarded. Media admission remains atomic. Discarded repair groups increment
`fec_dropped_resource`; configured reorder/NACK recovery still applies.
Increasing the ring is an explicit memory/protection tradeoff.

| Acceptance area | Reproduction / evidence | 0.2.0 final result |
|---|---|---|
| Seven profiles, both providers | `./scripts/ci-check.sh`; `/tmp/nanortc-ci-final-complete.log` | 54/54 checks pass: seven profiles × OpenSSL/mbedTLS, one-slot output, advanced features, feature-off builds and independent-peer interop. |
| Auth and negotiation rollback | `test_dtls`, `test_sdp`, `test_negotiation` | Targeted OpenSSL/mbedTLS suites pass, including related-address persistence and failure snapshots. |
| One pending transmit, stale tokens, retry | `test_output_lifetime`, `test_e2e`, example-I/O suites | Advanced-profile sanitizer and full CI checks pass, including the one-slot output configuration. |
| Media admission, stats and RTT | Media/e2e suites | Covered by passing advanced-profile sanitizer run, including accepted/sent counters and matching SR/RR RTT. |
| Established-session ICE restart and consent | `test_session_restart`, consent/e2e regressions | Two-peer host sessions in both initiating roles retain DC/audio/video, identity and sequence continuity without duplicate open/connect events. Old-route consent expiry stops traffic; inbound requests do not refresh consent. |
| Feedback cadence, wraps and weak-network behavior | `test_bwe_network`, BWE/TWCC suites | 50/100/200 ms simulations meet the 2 s decrease / 15 s recovery gates; fixed REMB cap and six independent probe-gate modes pass. Details below. |
| ASan + UBSan | All advanced media features enabled, OpenSSL and mbedTLS | 43/43 test suites pass on each provider; `/tmp/nanortc-ubsan-parameters-test.log` and `/tmp/nanortc-ubsan-mbedtls-test.log`. |
| Parser/controller fuzzing | 12 instrumented libFuzzer harnesses; 14 runs, including final SDP/FEC reruns | 96,285,341 executions; no failures or sanitizer errors. Each run targets 30 seconds, maximum input 4,096 bytes. |
| Configuration validation | `.cache/validation02/config-validation.json` | Eight invalid BWE threshold, percentage, interval and bucket configurations are rejected at compile time. |
| Memory / archive size | `scripts/measure-sizes.sh`, `test_sizeof` | Seven host and seven P4 profiles measured; original host ceilings pass. [Exact bytes](memory-profiles.md#020-measured-configurations-2026-10-13). |
| ESP-IDF examples / profiles | Installed IDF 6.1 / mbedTLS 4.1.0 builds | DC/ESP32, Media/S3, Advanced Media/S3, Camera/P4 and seven P4 profiles built and linked. No hardware flashing. |
| Browser | Chrome 147, both providers and offer/answer roles, MEDIA_H265 builds | Four sessions passed: 18 smoke DC echoes each, 4,096-byte message limit, trickle, Opus and H.264. Constrained-link results below; H.265 decode was not exercised. |
| Independent peer interoperability | Four libdatachannel DC/srflx/audio/video suites | All four pass after correcting test-fixture owner-lock starvation; video additionally passes three consecutive repetitions. |
| Test relay | Controlled UDP relay self-test | Passes; browser constrained-link tests use this relay. |
| Unvalidated runtime scope | Separate evidence required | External TURN deployment, hardware runtime and LeakSanitizer are not validated here. |

The deterministic weak-network model uses 50 ms RTT, a 40 ms queue limit,
1,000-byte packets and independent TWCC wire feedback. Capacity changes from
2 Mbit/s to 300 kbit/s at 3 seconds, then returns to 2 Mbit/s at 12 seconds.
Decrease time requires both target and the one-second sent-rate window to fall
to at most 360 kbit/s. Recovery requires the sent-rate window to reach
1.6 Mbit/s. Times below start at each capacity change, not at test startup.

| Feedback interval | Decrease | Recovery |
|---|---:|---:|
| 50 ms | 1,720 ms | 13,200 ms |
| 100 ms | 1,675 ms | 13,165 ms |
| 200 ms | 1,600 ms | 13,965 ms |

A separate persistent REMB vector keeps the target at or below 500 kbit/s.
Probe tests include a positive growth control, low offered load, growing queue
delay, local drops, only 75% history matches and retransmit ambiguity. These are
host simulations and failure gates, not measurements of device or Internet
latency.

Chrome also exercised a 2 Mbit/s → 300 kbit/s → 2 Mbit/s controlled relay in
all four provider/role sessions. Each constrained phase retained seven reliable
DC echoes and ongoing audio; the eight-second recovery phases produced eight
echoes and 199/201/198/202 decoded H.264 frames respectively. Estimates dropped
from 300 kbit/s to 138–163 kbit/s. After capacity returned, browser REMB caps
held the estimates around 138–163 kbit/s. These runs use fixed encoded samples
and do not adjust encoder bitrate, so they verify transport continuity and
decoded-video recovery. They do not establish real-encoder adaptation within
the separate 2 s/15 s goals.

Performance uses the same benchmark source against baseline `9af5849`, on an
Intel Core i5-14600K pinned to CPU 19, GCC 13.3/OpenSSL 3.0.13 Release, MEDIA_H265
with FEC off. Each value is the median of three run medians per revision; pair
order is before/after, after/before, before/after. Each run contains 11 batches
of 2,000 iterations. All six runs have checksum `817864513`. The video workload
is a 12 KB access unit with TWCC ID 3, SRTP protection, output drain and 0.2
transmit reports.

| Operation | Before | 0.2 | Change |
|---|---:|---:|---:|
| Four output copies/enqueues/drains | 195.0 ns | 229.0 ns | +17.44%; includes four new completion reports |
| RTP pack + SRTP protect | 2.4025 µs | 2.4165 µs | +0.58% |
| SCTP encode + CRC verification | 3.9685 µs | 3.9695 µs | +0.03% |
| H.264 access unit + SRTP + drain | 48.5615 µs | 42.6520 µs | −12.17% |
| H.265 access unit + SRTP + drain | 40.8900 µs | 40.7815 µs | −0.27%; effectively unchanged |

These are host batch averages, not packet-tail latency or device FPS. The small
H.265, SRTP and SCTP differences do not establish a meaningful CPU improvement.
The measured MEDIA_H265 state grows from 118,632 to 121,728 bytes, including
the new completion accounting and bounded transport history.

Compiler `-O3 -fstack-usage` reports H.265's old AP/FU helper frames at 1,312
bytes each and the new iterator frame at 128 bytes. `nanortc_send_video` changes
from 576 to 304 bytes, with separate 496-byte H.265 dispatch and 128-byte iterator
frames. These are individual frames, not total call-chain/task requirements.
The compatibility callback wrapper retains a 1,376-byte scratch frame; direct
video sends do not call it. Details and reproduction are in
[benchmarks/README.md](../../benchmarks/README.md).
Session artifacts: `.cache/validation02/benchmark/summary.json` and
`stack-summary.json`,
`/tmp/nanortc-02-final-sizes-related.txt`, `/tmp/nanortc-platform-final-summary.json` and
`/tmp/nanortc-esp-profiles-final.log`, `/tmp/nanortc-ci-final-complete.log` and
`/tmp/nanortc-ubsan-parameters-test.log`; fuzz summary and per-run logs are under
`.cache/validation02/fuzz/`. These local logs are not repository fixtures.

Earlier quality grades and browser/fuzz totals remain historical evidence; the
fresh browser scope is exactly the four sessions above. This record and
[memory-profiles.md](memory-profiles.md) contain the completed 0.2.0 acceptance
results within the stated limits.
