/*
 * nanortc interop tests — NanoRTC peer wrapper
 *
 * Runs a nanortc_t instance in a dedicated thread with a real UDP
 * socket, exchanging SDP/ICE with the remote peer via the signaling
 * pipe.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef INTEROP_NANORTC_PEER_H_
#define INTEROP_NANORTC_PEER_H_

#include "interop_common.h"
#include "run_loop.h"
#include "interop_owner.h"

#include <pthread.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Optional ICE server configuration for STUN/TURN-aware startup.
 * Pass NULL to interop_nanortc_start() for localhost-only mode. */
typedef struct {
    const nanortc_ice_server_t *ice_servers;
    size_t ice_server_count;
    void *resolve_scratch; /* Buffer for DNS resolution (caller-owned) */
    size_t resolve_scratch_size;
    /* When non-zero, skip the host candidate; the SDP will advertise only
     * the TURN relay candidate produced during warmup. Forces every byte
     * nanortc sends to be wrapped through the TURN server. */
    int relay_only;
    /* When non-zero, skip the host candidate and wait for the srflx
     * candidate (from STUN BIND) to be discovered before signaling.
     * Forces ICE pair selection through the srflx local candidate. */
    int srflx_only;
    /* When non-zero, require both srflx and relay candidates before signaling.
     * Used by strict real-network STUN+TURN discovery tests. */
    int require_srflx_and_relay;
} interop_nanortc_ice_config_t;

typedef struct {
    /* NanoRTC state */
    nanortc_t rtc;
    nano_run_loop_t loop;
    interop_owner_t owner; /* Serialize sends and hand off fairly after blocking I/O. */

    /* Signaling pipe (our end) */
    int sig_fd;

    /* Observable state (set from event callback, read from test thread) */
    atomic_int ice_connected;
    atomic_int dtls_connected;
    atomic_int sctp_connected;
    atomic_int dc_open;

    /* Last received DataChannel message */
    pthread_mutex_t msg_mutex;
    nanortc_stats_t stats_snapshot; /* Published by the RTC owner under msg_mutex. */
    char last_msg[4096];
    size_t last_msg_len;
    int last_msg_is_string;
    atomic_int msg_count;

    /* Thread */
    pthread_t thread;
    atomic_int running;
    uint16_t port;
    int has_ice;    /* True if ICE servers are configured (enables TURN warmup) */
    int relay_only; /* True if SDP must advertise only the TURN relay candidate */
    int srflx_only; /* True if SDP must advertise only the srflx candidate */
    int require_srflx_and_relay;
} interop_nanortc_peer_t;

/*
 * Start the nanortc peer in a background thread.
 *   - sig_fd: our end of the signaling pipe (fd[0])
 *   - port: UDP port to bind
 *   - ice_cfg: optional ICE server config (NULL = localhost-only, 127.0.0.1)
 *              When set, binds on 0.0.0.0, auto-detects real IP, and runs
 *              a TURN warmup phase before signaling.
 * Returns 0 on success.
 */
int interop_nanortc_start(interop_nanortc_peer_t *peer, int sig_fd, uint16_t port,
                          const interop_nanortc_ice_config_t *ice_cfg);

/*
 * Stop the peer thread and clean up.
 */
int interop_nanortc_stop(interop_nanortc_peer_t *peer);

/* Thread-safe copy of the most recently published owner-thread snapshot. */
nanortc_stats_t interop_nanortc_stats(interop_nanortc_peer_t *peer);
int interop_nanortc_send(interop_nanortc_peer_t *peer, uint16_t id, const void *data, size_t len);
int interop_nanortc_send_string(interop_nanortc_peer_t *peer, uint16_t id, const char *text);

/*
 * Wait until the given atomic flag becomes nonzero, or timeout.
 * Returns 0 on success, -1 on timeout.
 */
int interop_nanortc_wait_flag(atomic_int *flag, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* INTEROP_NANORTC_PEER_H_ */
