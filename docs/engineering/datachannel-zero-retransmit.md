# DataChannel zero-retransmission integration — 2026-09-07

PR #79 is integrated on the transport ownership model from #82. It retains
zero-retransmission policy and channel metadata without restoring the old
DCEP output latches, separate reassembly arena or copied receive-event slots.
The existing owned SCTP receive pool and message views cover those lifetimes.

## API and behavior

Zero-initialized options remain reliable and ordered. A positive
`max_retransmits` still selects retry-limited delivery. To explicitly request
zero retries:

```c
nanortc_datachannel_options_t options = {
    .protocol = "control.v1",
    .unordered = true,
    .partial_reliability = true,
    .max_retransmits = 0,
};
int channel = nanortc_create_datachannel(rtc, "control", &options);
```

DCEP OPEN retains protocol, ordering and retry policy. The open event exposes
`protocol`, `ordered`, `partial_reliability` and `max_retransmits`; protocol
and label pointers follow the existing next-mutation lifetime. Protocol names
must fit in `NANORTC_DC_LABEL_SIZE` including the terminating NUL; oversized
protocols fail instead of silently negotiating a truncated value. Retry limits
above 65535 and timed reliability remain unsupported. Custom callers must
recompile for the options/event/state ABI changes.

Both incoming and local zero-retry channels use the same send path. Sending
partial-reliability data requires `DC_RELIABLE` and negotiated Forward-TSN;
otherwise it returns `NOT_IMPLEMENTED`. DCEP control is sent reliably.

After an RTO, a zero-retry message is abandoned whole. A valid gap SACK can
trigger earlier abandonment when a later in-flight TSN is selectively ACKed.
This policy can discard reordered data too; a gap is not proof of loss. Already
gap-ACKed messages are retained, and their storage is not freed until cumulative
ACK because gap ACKs can be reneged. Gap ranges/list bounds are validated
before any queue change. Timeout and gap policies share whole-message
abandonment and the existing Forward-TSN retry path.

The old draft's legacy partial-PPID reassembly is not restored: RFC 8831 §6.6
deprecates that scheme; current message fragmentation stays at SCTP B/E level.
The final browser test uses current PPIDs, including empty string/binary framing.
This is still the bounded SCTP subset documented in the design, not full
congestion control or gap-SACK fast retransmission for reliable traffic.

## Validation

- Full local `scripts/ci-check.sh`: 54 checks passed, seven profiles and both
  crypto backends, original memory ceilings, ASan/LeakSanitizer and local interop.
- Combined ASan/UBSan with LeakSanitizer: 31/31 suites passed.
- Independent DCEP/SACK wire regressions cover protocol preservation,
  zero-vs-reliable policy, oversized protocol rejection, selective ACK handling
  and malformed SACK lists. Existing tests cover whole-message RTO abandonment,
  Forward-TSN retry, receive ownership, fragmentation and backpressure.
- SCTP fuzz: 1,000,000 executions under ASan/UBSan, no findings (34 seconds;
  LeakSanitizer disabled for the sandboxed fuzz process only).
- Chrome 147, OpenSSL/mbedTLS × offerer/answerer: 4/4 sessions, 72/72 exact echoes.
  The unordered channel uses `maxRetransmits: 0` and `protocol: 'control.v1'`;
  4096-byte, empty, UTF-8 and embedded-NUL messages pass. Each session receives
  Opus and decodes H.264. This is a local host-candidate test, not loss injection.
- Host and ESP-IDF 5.5.4/P4 profiles were remeasured. Existing state ceilings
  remain unchanged; see [memory-profiles.md](memory-profiles.md).

No ESP32 hardware or external TURN service validation was performed.

## References

- [RFC 8832 §5.1](https://www.rfc-editor.org/rfc/rfc8832.html#section-5.1): DCEP channel type, reliability parameter and protocol.
- [RFC 3758 §§3.1, 3.5](https://www.rfc-editor.org/rfc/rfc3758.html#section-3.5): partial reliability policy and whole-message abandonment.
- [RFC 9260 §3.3.4](https://www.rfc-editor.org/rfc/rfc9260.html#section-3.3.4): SACK gap representation.
- [RFC 8831 §6.6](https://www.rfc-editor.org/rfc/rfc8831.html#section-6.6): DataChannel messages and deprecated partial PPIDs.
