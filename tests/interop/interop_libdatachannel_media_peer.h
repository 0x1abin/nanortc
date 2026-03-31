/*
 * nanortc interop tests — libdatachannel media peer wrapper
 *
 * Wraps the libdatachannel C API as the offerer/controlling peer
 * with audio/video track support.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef INTEROP_LIBDATACHANNEL_MEDIA_PEER_H_
#define INTEROP_LIBDATACHANNEL_MEDIA_PEER_H_

#include "interop_common.h"
#include "interop_nanortc_media_peer.h" /* for interop_media_frame_t */

#include <pthread.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum tracks per peer */
#define LDC_MEDIA_MAX_TRACKS 2

/** Track type for libdatachannel media peer. */
typedef enum {
    LDC_TRACK_AUDIO = 0,
    LDC_TRACK_VIDEO = 1,
} ldc_track_kind_t;

/** Track codec for libdatachannel media peer. */
typedef enum {
    LDC_CODEC_OPUS = 0,
    LDC_CODEC_PCMA = 1,
    LDC_CODEC_PCMU = 2,
    LDC_CODEC_H264 = 3,
} ldc_codec_t;

/** Track direction for libdatachannel media peer. */
typedef enum {
    LDC_DIR_SENDRECV = 0,
    LDC_DIR_SENDONLY = 1,
    LDC_DIR_RECVONLY = 2,
} ldc_direction_t;

/** Configuration for a single media track to add. */
typedef struct {
    ldc_track_kind_t kind;
    ldc_direction_t direction;
    ldc_codec_t codec;
    uint32_t ssrc;        /* RTP SSRC (must be nonzero) */
    uint8_t payload_type; /* RTP payload type (e.g. 111 for Opus, 96 for H.264) */
} ldc_track_config_t;

typedef struct {
    /* libdatachannel handles */
    int pc;                           /* PeerConnection handle */
    int tracks[LDC_MEDIA_MAX_TRACKS]; /* Track handles (-1 if unused) */
    int track_count;

    /* Signaling pipe (our end) */
    int sig_fd;

    /* Observable state */
    atomic_int connected;  /* PeerConnection connected */
    atomic_int track_open; /* At least one track open */

    /* Received media frames */
    pthread_mutex_t frame_mutex;
    interop_media_frame_t frames[INTEROP_MEDIA_MAX_FRAMES];
    atomic_int frame_count;
    int frame_write_idx;

    /* Gathering state */
    atomic_int gathering_done;

    /* Remote nanortc port (for explicit candidate addition) */
    uint16_t remote_port;
} interop_libdatachannel_media_peer_t;

/*
 * Initialize libdatachannel, create PeerConnection, add media tracks,
 * generate an SDP offer, and exchange signaling.
 *
 *   - sig_fd: our end of the signaling pipe (fd[1])
 *   - track_configs: array of track configurations
 *   - track_count: number of tracks (1 or 2)
 *   - remote_port: nanortc's UDP port (for ICE candidate)
 *
 * Returns 0 on success.
 */
int interop_libdatachannel_media_start(interop_libdatachannel_media_peer_t *peer, int sig_fd,
                                       const ldc_track_config_t *track_configs, int track_count,
                                       uint16_t remote_port);

/*
 * Send media data on a track.
 *   - track_idx: index into the tracks array (0 or 1)
 *   - data: raw media payload (packetizer handles RTP)
 *   - len: payload length
 */
int interop_libdatachannel_media_send(interop_libdatachannel_media_peer_t *peer, int track_idx,
                                      const void *data, size_t len);

/*
 * Get the last received media frame.
 * Returns 0 on success, -1 if no frames received.
 */
int interop_libdatachannel_media_get_last_frame(interop_libdatachannel_media_peer_t *peer,
                                                interop_media_frame_t *out);

/*
 * Wait until a flag becomes nonzero, or timeout.
 * Returns 0 on success, -1 on timeout.
 */
int interop_libdatachannel_media_wait_flag(atomic_int *flag, int timeout_ms);

/*
 * Tear down libdatachannel media peer.
 */
int interop_libdatachannel_media_stop(interop_libdatachannel_media_peer_t *peer);

#ifdef __cplusplus
}
#endif

#endif /* INTEROP_LIBDATACHANNEL_MEDIA_PEER_H_ */
