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
#include "nano_rtc_internal.h"
#include "run_loop.h"

#include <pthread.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

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
} interop_nanortc_peer_t;

/*
 * Start the nanortc peer in a background thread.
 *   - sig_fd: our end of the signaling pipe (fd[0])
 *   - port: UDP port to bind (0 for auto)
 * Returns 0 on success.
 */
int interop_nanortc_start(interop_nanortc_peer_t *peer, int sig_fd,
                          uint16_t port);

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
