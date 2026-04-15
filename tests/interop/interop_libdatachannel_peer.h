/*
 * nanortc interop tests — libdatachannel peer wrapper
 *
 * Wraps the libdatachannel C API as the offerer/controlling peer.
 * libdatachannel manages its own internal threads; this wrapper
 * provides thread-safe observable state and signaling integration.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef INTEROP_LIBDATACHANNEL_PEER_H_
#define INTEROP_LIBDATACHANNEL_PEER_H_

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
    atomic_int connected; /* PeerConnection connected */
    atomic_int dc_open;   /* DataChannel open */

    /* Last received message */
    pthread_mutex_t msg_mutex;
    char last_msg[4096];
    size_t last_msg_len;
    int last_msg_is_string;
    atomic_int msg_count;

    /* Gathering state */
    atomic_int gathering_done;

    /* Remote nanortc port (for explicit candidate addition) */
    uint16_t remote_port;
} interop_libdatachannel_peer_t;

/*
 * Optional ICE configuration for the libdatachannel peer.
 *
 * Used by the relay-only TURN tests. Pass to interop_libdatachannel_start_ex().
 *
 *   - relay_only: if non-zero, sets rtcConfiguration.iceTransportPolicy =
 *     RTC_TRANSPORT_POLICY_RELAY, forcing libdc to advertise only its relay
 *     candidate. The peer becomes unreachable except via the configured TURN
 *     server.
 *   - stun_url / turn_url: non-NULL strings ("stun:host:port" / "turn:host:port").
 *     Either may be NULL to skip that server.
 *   - turn_user / turn_pass: long-term credentials for the TURN server. Both
 *     must be non-NULL when turn_url is set.
 */
typedef struct {
    int relay_only;
    const char *stun_url;
    const char *turn_url;
    const char *turn_user;
    const char *turn_pass;
} interop_libdatachannel_ice_config_t;

/*
 * Initialize libdatachannel, create PeerConnection, create a DataChannel
 * named `label`, generate an SDP offer, and send it through the signaling
 * pipe. Then wait for the SDP answer and ICE candidates.
 *
 *   - sig_fd: our end of the signaling pipe (fd[1])
 *   - label: DataChannel label (e.g. "test")
 *   - remote_port: nanortc's UDP port (for ICE candidate)
 *
 * Returns 0 on success. Equivalent to interop_libdatachannel_start_ex() with
 * a NULL ice_config (no ICE servers, host-only).
 */
int interop_libdatachannel_start(interop_libdatachannel_peer_t *peer, int sig_fd, const char *label,
                                 uint16_t remote_port);

/*
 * Like interop_libdatachannel_start() but accepts an ICE server / policy
 * configuration. Pass NULL ice_config for the legacy host-only behaviour.
 */
int interop_libdatachannel_start_ex(interop_libdatachannel_peer_t *peer, int sig_fd,
                                    const char *label, uint16_t remote_port,
                                    const interop_libdatachannel_ice_config_t *ice_config);

/*
 * Send a text message on the DataChannel.
 */
int interop_libdatachannel_send_string(interop_libdatachannel_peer_t *peer, const char *str);

/*
 * Send a binary message on the DataChannel.
 */
int interop_libdatachannel_send_binary(interop_libdatachannel_peer_t *peer, const void *data,
                                       size_t len);

/*
 * Wait until a flag becomes nonzero, or timeout.
 * Returns 0 on success, -1 on timeout.
 */
int interop_libdatachannel_wait_flag(atomic_int *flag, int timeout_ms);

/*
 * Tear down libdatachannel peer.
 */
int interop_libdatachannel_stop(interop_libdatachannel_peer_t *peer);

#ifdef __cplusplus
}
#endif

#endif /* INTEROP_LIBDATACHANNEL_PEER_H_ */
