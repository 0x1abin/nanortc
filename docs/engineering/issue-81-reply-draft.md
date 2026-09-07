# Draft reply to Issue #81

Not posted. This draft describes the implemented changes and their validation
boundaries; link both PRs when posting.

---

Thanks for the detailed report. We have prepared two PRs and found a real limitation in our ICE implementation:
only the controlling agent initiated connectivity checks. The controlled agent
mostly answered them. Discovering a STUN mapping alone does not open NAT filtering
toward the remote peer, so this could explain the behavior you observed. We have
not yet reproduced your particular WASI setup, however.

[PR #83](https://github.com/0x1abin/nanortc/pull/83) makes both roles send checks and
adds authenticated triggered checks, bounded remote peer-reflexive candidate
admission, retransmissions that retain their transaction IDs, and nomination only
after the relevant connectivity check succeeds. We also corrected the source
address handling so a mapped candidate uses its actual local socket/base.
The deterministic NAT regression fails on the old revision and connects in both
role assignments with the change, including bidirectional DataChannel messages.

The existing candidate flow remains the intended interface: consume
`NANORTC_EV_ICE_CANDIDATE` and pass its candidate string through your signaling
channel. If signaling queues the message asynchronously, copy the string before
the next output poll. The browser example now forwards these events, including srflx candidates
that arrive after SDP exchange. There is no need to bind a socket to the public
STUN-mapped address.

The separate example-hardening PR also makes explicit `-b` operation independent
of `getifaddrs`, removes the silent loopback fallback, moves the large application
and signaling buffers off the C stack, and ensures that UDP receive is nonblocking
while timers continue to run after EAGAIN/EINTR. We tested these paths natively,
including allocation/connection failures and both crypto backends with Chrome.

The out-of-bounds trap may involve the C stack, but that is still a hypothesis;
increasing the linear-memory limit alone does not establish the cause. Likewise,
we would not attribute the `recvfrom` behavior to wasi-libc without a small
reproducer. We have not validated WASI SDK 34 / Wasmtime 47 or your mbedTLS fork,
and these changes are not a claim of official WASI support.

Could you share the NanoRTC commit, build/link flags and adaptation patch, whether
the endpoints are behind different NATs or on the same LAN using the public
address, and the exchanged candidates plus ICE logs? Please redact credentials
and any addresses you do not want public. That will help determine whether the
remaining issue is connectivity checking, address advertisement, hairpinning or
runtime integration. Some NAT topologies will still require TURN.
