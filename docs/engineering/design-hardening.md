# Design hardening — 2026-09-07

Implementation record for the approved ownership, capacity and timer plan and
the follow-up review of uncommitted work and merged PRs #72, #73, #75, #76 and #78.
This is a source/ABI-breaking update; rebuild applications and custom crypto
providers together with the library.

## Review and convergence

The review found duplicated state and decisions in TURN scheduling, DCEP
pending output, RTC orchestration and SCTP fragment handling. The refactor
keeps the capacity checks, owned buffers and regression tests, while moving
each decision into its owning module:

- TURN sends and timeout queries use one task selector. An outstanding Refresh
  waits for its retransmission deadline (the reproduced case now returns
  400 ms instead of 0); an unarmed immediate refresh still returns 0.
- Response authentication precedes error dispatch. An unsigned 437 matching a
  live authenticated transaction is discarded without failing the allocation.
  Nonce rotation invalidates affected request IDs; their next sends use new
  IDs without resetting transmission budgets or concurrent retry deadlines.
  Continuous 438 challenges therefore cannot keep a transaction alive forever.
- RTC retains candidate order and TX-slot admission. TURN owns permission and
  channel readiness, retry/backoff and peer-specific failure isolation.
- DCEP channel pending state is the sole source of truth: peek into caller
  scratch, submit to SCTP, then commit. RTC no longer edits DCEP output fields.
- SCTP uses one fragment-identity/continuity predicate and bounded iterative
  in-place merging. Wire B/E/U flags are separate from internal delivery state;
  a caller-owned message view replaces stored delivery pointers and latches.
- Output polling is iterative and preserves priority and pointer lifetime.
  DTLS validates required provider operations in one place. Binary/text sends
  share a length-delimited internal path.

PR #79 remains independent. Its DataChannel API and SCTP changes overlap this
work; rebase it after this PR merges. This work retains `max_retransmits=0` as
reliable and does not add #79's zero-retransmission or protocol-metadata API.

## Changes and migration

| Area | Result | Application migration |
|---|---|---|
| SCTP encoding | Variable-length encoders take destination capacity and reject before writing; short output polls preserve queue/TSN state | Internal codec callers pass capacity |
| SCTP messages | Atomic message admission, MTU fragmentation, B/E reassembly, owned receive pool, independent stream SSNs, bundled DATA delivered one message per poll | Drain outputs; copy event payloads before the next mutating call |
| Message limits | `NANORTC_SCTP_MAX_MESSAGE_SIZE` follows the receive limit: 4096 B on host, 2048 B in default ESP profiles; SDP advertises this value and sends honor the peer limit | Configure pools and message limit at compile time; distinguish permanent `BUFFER_TOO_SMALL` from temporary `WOULD_BLOCK` |
| Removed runtime fields | `sctp_send_buf_size` / `sctp_recv_buf_size` did not size embedded arrays and are removed | Use `NANORTC_SCTP_SEND_BUF_SIZE`, `NANORTC_SCTP_RECV_BUF_SIZE` and `NANORTC_SCTP_RECV_GAP_BUF_SIZE` |
| DataChannel | Per-channel pending OPEN/ACK, actual unordered flag and retry limit; retry exhaustion abandons all message fragments and sends/retries Forward TSN | `max_retransmits=0` retains the public API's reliable default; positive retry limits require `DC_RELIABLE` and peer Forward-TSN support |
| Text payloads | Added `nanortc_datachannel_send_text(..., str, len)`; Linux/ESP32 echo examples use the received length | Received text has no NUL-termination guarantee; use the length-delimited API to preserve UTF-8 and embedded NUL characters |
| Output ownership | Candidates keep identity until poll-time formatting; full queues retain scalar metadata by event type and track/channel | Repeated scalar notifications may coalesce to the latest snapshot; DataChannel payloads remain in the SCTP receive pool |
| Logging | Instance callback passed explicitly; stateless parsers have no ambient logger | Configure each instance independently; protocol failures are logged at RTC boundaries |
| DTLS timers | Required provider `dtls_set_time`, `dtls_next_timeout`, `dtls_handle_timeout`; packet and timer paths share handshake/key export | Custom providers must implement all three; remaining timeout is relative milliseconds, `UINT32_MAX` means unarmed, timeout handling returns 0/done, 1/wait or negative failure |
| Event loop | Removed `NANORTC_OUTPUT_TIMEOUT`; deadline query includes DTLS and audio jitter, alongside existing timers | Drain TRANSMIT/EVENT, query `nanortc_next_timeout_ms`, wait, then feed current time through `nanortc_handle_input`; shared Linux/ESP/session-loop examples migrated |
| NACK | Retransmit history matches both media-source SSRC and sequence number | No application change |
| CI | Local and GitHub checks inspect local `b/d` as well as exported `B/D` mutable core symbols | `bash scripts/check-mutable-state.sh build/libnanortc.a` |

mbedTLS retransmissions use the caller clock, including wraparound. OpenSSL 3.0
retains its internal real clock: the adapter uses `DTLSv1_get_timeout` and
`DTLSv1_handle_timeout`. OpenSSL timer tests wait real time; mbedTLS tests advance
virtual time without sleeping.

Local creation with a nonempty DataChannel sub-protocol and timed-reliability
OPEN types return `NOT_IMPLEMENTED`. Incoming OPEN sub-protocol bytes are
parsed but are not exposed or negotiated by the current public API. The SCTP
implementation still lacks full congestion control, gap-SACK fast retransmit,
complete graceful shutdown and INIT/COOKIE retransmission. This change does not
claim complete RFC 9260 compliance.

## Verification

Regression coverage includes destination canaries and whole-state snapshots,
maximum-size fragmentation, out-of-order/duplicate fragments, multiple bundled
messages, many small fragments, oversized continuations, distinct stream IDs,
whole-message abandonment and Forward-TSN retry, pending DCEP OPENs, full output
queues, candidate identity, two-instance logging, jitter tail/wraparound, and
same-sequence NACKs belonging to different SSRCs. DTLS lost-first-flight tests
exercise both providers through timeout handling and finish a handshake. TURN
regressions cover refresh waiting, untrusted errors, concurrent nonce rotation,
late responses, repeated 438, backpressure, clock wraparound and RNG failure.
Independent SCTP wire tests reject inconsistent fragment identities and verify
that reserved DATA bits cannot act as internal Forward-TSN delivery state.

The interop large-message case now sends the configured 4 KiB limit in both
directions against libdatachannel. Protocol layouts and behavior come from RFCs;
libdatachannel is used only as an independent test peer.

Final results on the completed source:

- Full `scripts/ci-check.sh`: **passed**, including all 7 feature combinations ×
  OpenSSL/mbedTLS, architectural/format checks, all-advanced ASan with
  LeakSanitizer, and additional media/feature variants.
- Combined ASan + UBSan all-advanced profile: **31/31 tests passed**, with
  `-fsanitize=undefined -fno-sanitize-recover=undefined` and `ADDRESS_SANITIZER=ON`.
- Local interop: **4/4 passed** (DataChannel including bidirectional 4096 B,
  srflx, audio and video). External TURN/network-labelled tests are excluded
  by the standard CI command and were not run.
- Final stateful fuzz under ASan/UBSan: **SCTP 1,000,000 executions in 23 seconds**
  and **TURN 1,000,000 in 7 seconds**, no findings. The TURN harness activates
  matching transactions and drives bounded timer/reply steps, including wraparound.
  Leak detection was disabled only for fuzz because of sandbox ptrace limits;
  the full ASan/LeakSanitizer suite ran successfully outside that sandbox.
- Linux examples: browser interop and camera targets built successfully,
  including both modified Linux/common event loops. No live camera run.
- Mutable-state check: an independent archive containing file-local
  `static int counter` was correctly rejected as a lowercase `b` symbol.
- Existing structure-size ceilings all pass without increasing the limits.

### Chrome follow-up

Chrome **147.0.7727.137** passed **4/4 real browser sessions**:
OpenSSL/mbedTLS × NanoRTC offerer/answerer. Each session opened an ordered,
reliable channel and an unordered channel with `maxRetransmits=2`; all **72**
binary/text echoes matched byte-for-byte. Cases include empty messages,
4096-byte fragmentation in both directions, multibyte UTF-8, short text after
large messages, and embedded NUL characters. Every session decoded at least
10 H.264 frames and received at least 19,200 Opus samples. These are headless
decoder/statistics checks, not subjective audio/video quality assessment.

The first browser run exposed a real example-layer bug: after a 4096-byte
binary message, a short text echo timed out because `send_string()` read beyond
the received message looking for a terminator. Both Linux and ESP32 examples
now use the new length-delimited text API. A byte-level E2E regression checks
nonterminated text, embedded NULs and empty-text PPID framing. Full host CI and
the 31-test combined ASan/UBSan suite passed again after the fix.

Reproduce with Node.js 22+ and installed Chrome (no npm dependencies):

```bash
node scripts/test-browser.mjs build/examples/browser_interop/browser_interop --media
```

Build with audio/video enabled for `--media`; omit it for DataChannel-only
builds. Repeat with each crypto backend. Firefox/Safari and real-link packet
loss were not exercised by this smoke test.

### ESP-IDF compilation

ESP-IDF **v5.5.4**, loaded from the user-provided
`/home/zzb/workspace/esp/esp-idf`, includes mbedTLS **3.6.5**. All four target
example builds passed, including final linking and partition-size checks:

| Target | Example/profile | Application image bytes |
|---|---|---:|
| ESP32-S3 | DataChannel defaults | 933,648 |
| ESP32-S3 | Media defaults | 3,500,528 |
| ESP32-S3 | Media `sdkconfig.defaults.ci-advanced` | 3,509,392 |
| ESP32-P4 | Camera, existing `esp32_p4_nano` board | 2,230,992 |

The advanced profile enables H.265, rate control, reordering, NACK reception,
FEC, pacing and automatic PLI. The camera build uses its declared managed
components and existing board generator. Existing defaults emit warnings for
three obsolete Kconfig symbols (`MBEDTLS_X509_CREATE_C`,
`MBEDTLS_X509_CRT_WRITE_C`, `LWIP_SO_RCVTIMEO`); these did not prevent the builds.

All **7 ESP32-P4 feature profiles** also compiled and linked in the measurement
project. Exact state sizes and archive code/read-only data sizes are in
[memory-profiles.md](memory-profiles.md). The measurement script now covers
H.265, uses one profile table for host/IDF builds, isolates each build and
SDK configuration under `.cache/measure-sizes`, reports exact bytes plus KiB
and the actual optimization/size accounting, and disables
IDF's asynchronous log hints to avoid stalled unattended build wrappers.

These are final-source cross-compilation results for all four examples. ESP32 hardware validation is **excluded from
this task at the user's request**; no on-device heap/stack or packet-loss
results are claimed.

## Host memory

64-bit host defaults, OpenSSL. These are current `sizeof(nanortc_t)`
measurements. Code + read-only data is GNU `size`'s `text` column summed over
the Release (`-O3 -DNDEBUG`) archive, including the OpenSSL adapter but excluding
the dynamically linked OpenSSL library. This matches the measurement script;
it is broader than the earlier `.text*`-only accounting. These are not target heap/stack/flash measurements. Existing CI ceilings are
unchanged. Candidate formatting shares receive/TURN scratch; reassembly rotates
bytes inside the receive pool instead of adding a second 4 KiB message buffer.
Metadata capacity is derived from enabled features and bounded entity counts.

| Profile | `sizeof(nanortc_t)` bytes | Host code + read-only data bytes |
|---|---:|---:|
| CORE_ONLY | 20,640 | 88,872 |
| DATA | 35,520 | 125,284 |
| AUDIO_ONLY | 46,400 | 114,897 |
| AUDIO | 61,272 | 150,965 |
| MEDIA_ONLY | 102,472 | 127,401 |
| MEDIA | 117,352 | 164,077 |
| MEDIA_H265 | 118,392 | 171,981 |

Against the pre-refactor hardening snapshot, host DataChannel profiles save
152 B (160 B for AUDIO); AUDIO_ONLY grows 8 B due to TURN state/alignment.
On P4, all DataChannel profiles save 144 B; CORE_ONLY and AUDIO_ONLY grow 8 B.
Other state sizes are unchanged. No structure-size ceiling was raised.

CORE_ONLY reaches its existing ceiling; future core additions need a fresh size
check. Target measurements are recorded separately in
[memory-profiles.md](memory-profiles.md).

## Protocol references

- RFC 9260 §§3.3.1, 6.2, 6.5, 6.9: DATA layout, receive ownership, stream ordering and fragmentation.
- RFC 3758 §§3.3, 3.5, 3.6: Forward-TSN negotiation, whole-message abandonment and receiver processing.
- RFC 8831 §6.6 and RFC 8832 §§5–6: DataChannel messages, empty PPIDs and DCEP.
- RFC 8841 §6: SDP maximum message size, absent default 65536 and zero meaning no remote limit.
- RFC 4585 §6.2.1: NACK media-source SSRC and PID/BLP.
- RFC 6347 §4.2.4: DTLS flight retransmission.
- [RFC 8489 §§5, 6.2.1, 9.2.5](https://www.rfc-editor.org/rfc/rfc8489.html#section-9.2.5): transaction identity, retransmission and response authentication.
- [RFC 8656 §§8–12](https://www.rfc-editor.org/rfc/rfc8656.html#section-8): allocation refresh, permissions and channels.
