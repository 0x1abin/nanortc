# Real TURN Interop CI

NanoRTC's strict TURN tests use a real UDP relay and fail closed. They are
separate from pull-request CI because TURN REST authentication depends on a
shared server secret.

## Production-relay safety boundary

- A dedicated coturn instance and secret remain the preferred CI topology. If
  an existing production relay is used, keep this workflow manual-only and
  protected: each run creates real allocations and consumes relay bandwidth.
- Rotate any secret that has appeared in chat, logs, shell history, or workflow
  output at the next safe maintenance window. Rotation is an operator action,
  not part of this test workflow.
- Do not give the GitHub runner SSH access. It needs only UDP access to the TURN
  listener and relay-port range.
- In a public repository, never expose the secret to `pull_request` or
  `pull_request_target`. The workflow is intentionally limited to protected
  manual dispatch and has one repository-wide concurrency slot.
- The workflow does not change coturn configuration, restart the service,
  modify firewall rules, or use SSH. Test allocations expire normally.

## Coturn profile

The supported automated profile is IPv4 TURN over UDP with REST-style
time-limited credentials. An existing compatible coturn can be used without
configuration changes. For a new dedicated test deployment, a minimal profile
is:

```ini
listening-port=3478
fingerprint
use-auth-secret
static-auth-secret=<rotated-ci-only-secret>
realm=turn-ci.example.com
stale-nonce=600
no-cli
min-port=49160
max-port=49260
```

Set `external-ip` when coturn is behind one-to-one NAT. Allow inbound UDP 3478
and the configured relay range; restrict administration/SSH separately. Store
the configuration as root-owned and group-readable only by the turnserver
service account (for example `0640 root:turnserver`).

## GitHub environment

Create a protected environment named `turn-interop`:

| Kind | Name | Value |
|------|------|-------|
| Secret | `COTURN_AUTH_SECRET` | Coturn REST shared secret |
| Variable | `NANORTC_TURN_URL` | `turn:turn.example.com:3478` |
| Variable | `NANORTC_STUN_URL` | Optional matching `stun:` URL |

Require a reviewer for the environment. The workflow has no push, pull-request,
or schedule trigger: an authorized operator must dispatch it and approve the
environment deployment. It builds before accessing the secret, then derives a
30-minute username/password, masks the password, and passes only those derived
values to CTest.

Run the workflow only during a suitable production window. The tests are small
and bounded, but they deliberately establish relay-to-relay ICE, DTLS, and
DataChannel traffic, so they are not zero-load probes.

## Running locally

Build interop tests, then provide an already-derived TURN username/password:

```bash
cmake -B build-interop \
  -DNANORTC_BUILD_INTEROP_TESTS=ON \
  -DNANORTC_CRYPTO=openssl
cmake --build build-interop -j$(nproc)

export NANORTC_INTEROP_NETWORK_REQUIRED=1
export NANORTC_TURN_URL=turn:turn.example.com:3478
export NANORTC_STUN_URL=stun:turn.example.com:3478
export NANORTC_TURN_USER="$TURN_USER"
export NANORTC_TURN_PASS="$TURN_PASS"
ctest --test-dir build-interop -R '^interop_turn$' --output-on-failure
ctest --test-dir build-interop -L turn-relay --output-on-failure
```

Without strict mode, unavailable external-network tests return skip code 77.
The regular `scripts/ci-check.sh` deliberately runs interop with `-LE network`.
