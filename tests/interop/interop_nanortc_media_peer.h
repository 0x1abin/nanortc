/*
 * nanortc interop tests — NanoRTC media peer wrapper
 *
 * Runs a nanortc_t instance with audio/video tracks in a dedicated
 * thread with a real UDP socket, exchanging SDP/ICE with the remote
 * peer via the signaling pipe.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef INTEROP_NANORTC_MEDIA_PEER_H_
#define INTEROP_NANORTC_MEDIA_PEER_H_

#include "interop_common.h"
#include "run_loop.h"

#include <pthread.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of media frames buffered for verification */
#define INTEROP_MEDIA_MAX_FRAMES 32
#define INTEROP_MEDIA_FRAME_SIZE 2048

/** Configuration for a single media track to add before offer/answer. */
typedef struct {
    nanortc_track_kind_t kind;     /* NANORTC_TRACK_AUDIO or NANORTC_TRACK_VIDEO */
    nanortc_direction_t direction; /* SENDRECV, SENDONLY, RECVONLY */
    nanortc_codec_t codec;         /* e.g. NANORTC_CODEC_OPUS, NANORTC_CODEC_H264 */
    uint32_t sample_rate;          /* Audio sample rate (0 for video) */
    uint8_t channels;              /* Audio channels (0 for video) */
} interop_media_track_config_t;

/** Received media frame stored for test verification. */
typedef struct {
    uint8_t data[INTEROP_MEDIA_FRAME_SIZE];
    size_t len;
    uint8_t mid;
    uint32_t timestamp;
    bool is_keyframe;
} interop_media_frame_t;

typedef struct {
    /* NanoRTC state */
    nanortc_t rtc;
    nano_run_loop_t loop;

    /* Signaling pipe (our end) */
    int sig_fd;

    /* Track configuration */
    interop_media_track_config_t tracks[2];
    int track_count;
    int track_mids[2]; /* MIDs returned by nanortc_add_*_track() */

    /* Observable state (set from event callback, read from test thread) */
    atomic_int ice_connected;
    atomic_int dtls_connected;
    atomic_int connected;
    atomic_int media_added;

    /* Received media frames (ring buffer) */
    pthread_mutex_t frame_mutex;
    interop_media_frame_t frames[INTEROP_MEDIA_MAX_FRAMES];
    atomic_int frame_count; /* total frames received */
    int frame_write_idx;    /* next write position in ring buffer */

    /* Thread */
    pthread_t thread;
    atomic_int running;
    uint16_t port;
} interop_nanortc_media_peer_t;

/*
 * Start the nanortc media peer in a background thread.
 *   - sig_fd: our end of the signaling pipe (fd[0])
 *   - port: UDP port to bind
 *   - tracks: array of track configs to add before accept_offer
 *   - track_count: number of tracks (1 or 2)
 * Returns 0 on success.
 */
int interop_nanortc_media_start(interop_nanortc_media_peer_t *peer, int sig_fd, uint16_t port,
                                const interop_media_track_config_t *tracks, int track_count);

/*
 * Stop the peer thread and clean up.
 */
int interop_nanortc_media_stop(interop_nanortc_media_peer_t *peer);

/*
 * Send audio on the given track MID.
 * Thread-safe (nanortc_send_audio is re-entrant for output queuing).
 */
int interop_nanortc_media_send_audio(interop_nanortc_media_peer_t *peer, uint8_t mid,
                                     uint32_t pts_ms, const void *data, size_t len);

/*
 * Send video on the given track MID.
 */
int interop_nanortc_media_send_video(interop_nanortc_media_peer_t *peer, uint8_t mid,
                                     uint32_t pts_ms, const void *data, size_t len);

/*
 * Get the last received media frame (copies into caller's buffer).
 * Returns 0 on success, -1 if no frames received.
 */
int interop_nanortc_media_get_last_frame(interop_nanortc_media_peer_t *peer,
                                         interop_media_frame_t *out);

#ifdef __cplusplus
}
#endif

#endif /* INTEROP_NANORTC_MEDIA_PEER_H_ */
