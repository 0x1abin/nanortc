/*
 * nanortc interop tests — libdatachannel peer wrapper
 *
 * Wraps the libdatachannel C API as the offerer/controlling peer.
 * libdatachannel manages its own internal threads; this wrapper
 * provides thread-safe observable state and signaling integration.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef INTEROP_LIBDC_PEER_H_
#define INTEROP_LIBDC_PEER_H_

#include "interop_common.h"

#include <pthread.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* libdatachannel handles */
    int pc; /* PeerConnection handle */
    int dc; /* DataChannel handle (-1 if none) */

    /* Signaling pipe (our end) */
    int sig_fd;

    /* Observable state */
    atomic_int connected;  /* PeerConnection connected */
    atomic_int dc_open;    /* DataChannel open */

    /* Last received message */
    pthread_mutex_t msg_mutex;
    char last_msg[4096];
    size_t last_msg_len;
    int last_msg_is_string;
    atomic_int msg_count;

    /* Gathering state */
    atomic_int gathering_done;
} interop_libdc_peer_t;

/*
 * Initialize libdatachannel, create PeerConnection, create a DataChannel
 * named `label`, generate an SDP offer, and send it through the signaling
 * pipe. Then wait for the SDP answer and ICE candidates.
 *
 *   - sig_fd: our end of the signaling pipe (fd[1])
 *   - label: DataChannel label (e.g. "test")
 *   - remote_port: nanortc's UDP port (for ICE candidate)
 *
 * Returns 0 on success.
 */
int interop_libdc_start(interop_libdc_peer_t *peer, int sig_fd,
                        const char *label, uint16_t remote_port);

/*
 * Send a text message on the DataChannel.
 */
int interop_libdc_send_string(interop_libdc_peer_t *peer, const char *str);

/*
 * Send a binary message on the DataChannel.
 */
int interop_libdc_send_binary(interop_libdc_peer_t *peer, const void *data,
                              size_t len);

/*
 * Wait until a flag becomes nonzero, or timeout.
 * Returns 0 on success, -1 on timeout.
 */
int interop_libdc_wait_flag(atomic_int *flag, int timeout_ms);

/*
 * Tear down libdatachannel peer.
 */
int interop_libdc_stop(interop_libdc_peer_t *peer);

#ifdef __cplusplus
}
#endif

#endif /* INTEROP_LIBDC_PEER_H_ */
