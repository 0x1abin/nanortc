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
} interop_nanortc_ice_config_t;

typedef struct {
    /* NanoRTC state */
    nanortc_t rtc;
    nano_run_loop_t loop;

    /* Signaling pipe (our end) */
    int sig_fd;

    /* Observable state (set from event callback, read from test thread) */
    atomic_int ice_connected;
    atomic_int dtls_connected;
    atomic_int sctp_connected;
    atomic_int dc_open;

    /* Last received DataChannel message */
    pthread_mutex_t msg_mutex;
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

/*
 * Wait until the given atomic flag becomes nonzero, or timeout.
 * Returns 0 on success, -1 on timeout.
 */
int interop_nanortc_wait_flag(atomic_int *flag, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* INTEROP_NANORTC_PEER_H_ */
