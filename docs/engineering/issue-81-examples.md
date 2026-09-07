# Issue #81: host example portability

This change is independent of [the ICE fix, PR #83](https://github.com/0x1abin/nanortc/pull/83).
It improves the native example boundary without asserting that the reporter's
WASI out-of-bounds trap or receive behavior has been reproduced.

## Changes

- CMake detects `getifaddrs`; `NANO_HAVE_GETIFADDRS=OFF` builds the explicit-address
  fallback. No-interface and discovery-failure paths report an error. `-b` is the
  local advertised candidate, while the single socket remains wildcard-bound.
- UDP receives use `MSG_DONTWAIT`, falling back to `O_NONBLOCK`. Readiness races,
  interrupted waits and empty datagrams still drive timers. Hard I/O errors
  reach the caller. IPv4 sockets use native IPv4 addresses in dual-stack builds.
- Browser application state, SDP/payload and video scratch live in one allocated
  workspace, released through guarded cleanup. HTTP send/send-to share one
  implementation; their temporary 16 KiB buffer and the receive buffer are
  released on all exits. No library heap allocation or public API change.
- Local candidate events are sent over the signaling relay, including srflx
  candidates discovered after SDP exchange. Logs include role and selected path.
- The Chrome harness uses bounded filesystem-removal retries after shutdown;
  Chrome helper processes can briefly retain their profile after parent exit.

The HTTP client remains blocking. Candidate signaling and improved stack use
complement the ICE fix; they do not independently guarantee NAT traversal or
establish WASI support. ESP32 hardware and external TURN service checks are
outside this revision.

## Validation

`test_example_io` and `test_example_io_fcntl` compile the actual shared helpers
with deterministic syscall/allocator shims, without real sockets or scheduling
races. They exercise absent enumeration, both nonblocking methods, EAGAIN,
EWOULDBLOCK, EINTR and hard errors, timer progress, socket-family conversion,
allocation failure, connection failure, oversized signaling payloads and
successful HTTP send/receive cleanup.

The no-enumeration native build completes both Chrome roles with explicit `-b`,
18 echo cases (4096-byte, empty, UTF-8 and embedded NUL), Opus reception and
H.264 decoding. Without `-b` it exits before contacting signaling and reports
the missing address.

GCC `-fstack-usage`, native DATA+AUDIO+VIDEO build without optimization:
`main` 3,248 bytes; answer/offer signaling 128 bytes each; trickle polling
112 bytes; HTTP receive 416 bytes; shared HTTP send 976 bytes. These are
individual host function frames, not total stack usage or WASI measurements.

Final native validation (2026-09-07): `scripts/ci-check.sh` **54/54**;
combined ASan/UBSan **33/33** suites. Chrome **147.0.7727.137** passed both roles
with OpenSSL (`getifaddrs` disabled) and mbedTLS (normal detection): **18** echo
cases and at least one signaled local candidate per session, with received Opus
samples and decoded H.264 frames. The runner exited successfully after cleanup.
No ESP32 hardware, external TURN service or WASI runtime validation was performed.
