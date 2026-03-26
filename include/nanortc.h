/*
 * nanortc — Sans I/O WebRTC for RTOS/embedded systems
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_H_
#define NANORTC_H_

#include "nanortc_config.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------
 * Public API visibility
 * ---------------------------------------------------------------- */

#if defined(__GNUC__) || defined(__clang__)
#define NANO_API __attribute__((visibility("default")))
#else
#define NANO_API
#endif

/* ----------------------------------------------------------------
 * Version
 * ---------------------------------------------------------------- */

#define NANORTC_VERSION_MAJOR 0
#define NANORTC_VERSION_MINOR 1
#define NANORTC_VERSION_PATCH 0

/* ----------------------------------------------------------------
 * Build Profiles
 * ---------------------------------------------------------------- */

#define NANO_PROFILE_DATA  1 /* DataChannel only */
#define NANO_PROFILE_AUDIO 2 /* DataChannel + audio (RTP/SRTP) */
#define NANO_PROFILE_MEDIA 3 /* DataChannel + audio + video */

/* NANORTC_PROFILE default is set in nanortc_config.h */

/* ----------------------------------------------------------------
 * Self-contained byte order (no platform htons/ntohs)
 * ---------------------------------------------------------------- */

static inline uint16_t nano_htons(uint16_t x)
{
    return (uint16_t)((x >> 8) | (x << 8));
}

static inline uint16_t nano_ntohs(uint16_t x)
{
    return nano_htons(x);
}

static inline uint32_t nano_htonl(uint32_t x)
{
    return ((x >> 24) & 0x000000FFu) | ((x >> 8) & 0x0000FF00u) | ((x << 8) & 0x00FF0000u) |
           ((x << 24) & 0xFF000000u);
}

static inline uint32_t nano_ntohl(uint32_t x)
{
    return nano_htonl(x);
}

/* ----------------------------------------------------------------
 * Error Codes
 * ---------------------------------------------------------------- */

#define NANO_OK                   0
#define NANO_ERR_INVALID_PARAM    -1
#define NANO_ERR_BUFFER_TOO_SMALL -2
#define NANO_ERR_STATE            -3
#define NANO_ERR_CRYPTO           -4
#define NANO_ERR_PROTOCOL         -5
#define NANO_ERR_NOT_IMPLEMENTED  -6
#define NANO_ERR_PARSE            -7
#define NANO_ERR_NO_DATA          -8
#define NANO_ERR_INTERNAL         -9

/* Configuration limits are defined in nanortc_config.h */

/* ----------------------------------------------------------------
 * Forward declarations
 * ---------------------------------------------------------------- */

typedef struct nano_crypto_provider nano_crypto_provider_t;

/* ----------------------------------------------------------------
 * Address type (network-agnostic)
 * ---------------------------------------------------------------- */

typedef struct nano_addr {
    uint8_t family; /* 4 = IPv4, 6 = IPv6 */
    uint8_t addr[16];
    uint16_t port;
} nano_addr_t;

/* ----------------------------------------------------------------
 * Output / Event enums
 * ---------------------------------------------------------------- */

typedef enum {
    NANO_OUTPUT_TRANSMIT, /* UDP data to send */
    NANO_OUTPUT_EVENT,    /* Application event */
    NANO_OUTPUT_TIMEOUT,  /* Next timeout in ms */
} nano_output_type_t;

typedef enum {
    /* Connection lifecycle */
    NANO_EVENT_ICE_CONNECTED,
    NANO_EVENT_DTLS_CONNECTED,
    NANO_EVENT_SCTP_CONNECTED,
    NANO_EVENT_DISCONNECTED,

    /* DataChannel (all profiles) */
    NANO_EVENT_DATACHANNEL_OPEN,
    NANO_EVENT_DATACHANNEL_CLOSE,
    NANO_EVENT_DATACHANNEL_DATA,
    NANO_EVENT_DATACHANNEL_STRING,

#if NANORTC_PROFILE >= NANO_PROFILE_AUDIO
    /* Audio (AUDIO / MEDIA profiles) */
    NANO_EVENT_AUDIO_DATA,
#endif

#if NANORTC_PROFILE >= NANO_PROFILE_MEDIA
    /* Video (MEDIA profile) */
    NANO_EVENT_VIDEO_DATA,
    NANO_EVENT_KEYFRAME_REQUEST,
#endif
} nano_event_type_t;

/* ----------------------------------------------------------------
 * Event structure
 * ---------------------------------------------------------------- */

typedef struct nano_event {
    nano_event_type_t type;
    uint16_t stream_id; /* DataChannel stream ID */
    const uint8_t *data;
    size_t len;
    uint32_t timestamp; /* RTP timestamp (media events) */
    bool is_keyframe;   /* video only */
} nano_event_t;

/* ----------------------------------------------------------------
 * Output structure
 * ---------------------------------------------------------------- */

typedef struct nano_output {
    nano_output_type_t type;
    union {
        struct {
            const uint8_t *data;
            size_t len;
            nano_addr_t dest;
        } transmit;
        nano_event_t event;
        uint32_t timeout_ms;
    };
} nano_output_t;

/* ----------------------------------------------------------------
 * Direction (media)
 * ---------------------------------------------------------------- */

typedef enum {
    NANO_DIR_SENDRECV,
    NANO_DIR_SENDONLY,
    NANO_DIR_RECVONLY,
    NANO_DIR_INACTIVE,
} nano_direction_t;

/* ----------------------------------------------------------------
 * Codec identifiers
 * ---------------------------------------------------------------- */

typedef enum {
    NANO_CODEC_NONE = 0,
    NANO_CODEC_OPUS,
    NANO_CODEC_PCMA, /* G.711 A-law */
    NANO_CODEC_PCMU, /* G.711 mu-law */
    NANO_CODEC_H264,
    NANO_CODEC_VP8,
} nano_codec_t;

/* ----------------------------------------------------------------
 * ICE role
 * ---------------------------------------------------------------- */

typedef enum {
    NANO_ROLE_CONTROLLED,  /* answerer: respond to STUN checks (ICE-Lite) */
    NANO_ROLE_CONTROLLING, /* offerer: initiate STUN checks */
} nano_ice_role_t;

/* ----------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------- */

typedef struct nano_rtc_config {
    const nano_crypto_provider_t *crypto;
    nano_ice_role_t role;

    /* Memory configuration */
    uint32_t sctp_send_buf_size;
    uint32_t sctp_recv_buf_size;

#if NANORTC_PROFILE >= NANO_PROFILE_AUDIO
    nano_codec_t audio_codec;
    uint32_t audio_sample_rate;
    uint8_t audio_channels;
    nano_direction_t audio_direction;
    uint32_t jitter_depth_ms;
#endif

#if NANORTC_PROFILE >= NANO_PROFILE_MEDIA
    nano_codec_t video_codec;
    nano_direction_t video_direction;
#endif
} nano_rtc_config_t;

/* ----------------------------------------------------------------
 * Main state machine (opaque internals)
 * ---------------------------------------------------------------- */

/* Forward-declare internal struct; defined in nano_rtc_internal.h */
typedef struct nano_rtc nano_rtc_t;

/* ----------------------------------------------------------------
 * Lifecycle API
 * ---------------------------------------------------------------- */

NANO_API int nano_rtc_init(nano_rtc_t *rtc, const nano_rtc_config_t *cfg);
NANO_API void nano_rtc_destroy(nano_rtc_t *rtc);

/* ----------------------------------------------------------------
 * SDP API
 * ---------------------------------------------------------------- */

NANO_API int nano_accept_offer(nano_rtc_t *rtc, const char *offer, char *answer_buf,
                               size_t answer_buf_len);
NANO_API int nano_create_offer(nano_rtc_t *rtc, char *offer_buf, size_t offer_buf_len);
NANO_API int nano_accept_answer(nano_rtc_t *rtc, const char *answer);

/* ----------------------------------------------------------------
 * ICE API
 * ---------------------------------------------------------------- */

NANO_API int nano_add_local_candidate(nano_rtc_t *rtc, const char *ip, uint16_t port);
NANO_API int nano_add_remote_candidate(nano_rtc_t *rtc, const char *candidate_str);

/* ----------------------------------------------------------------
 * Event loop (Sans I/O core)
 * ---------------------------------------------------------------- */

NANO_API int nano_poll_output(nano_rtc_t *rtc, nano_output_t *out);
NANO_API int nano_handle_receive(nano_rtc_t *rtc, uint32_t now_ms, const uint8_t *data, size_t len,
                                 const nano_addr_t *src);
NANO_API int nano_handle_timeout(nano_rtc_t *rtc, uint32_t now_ms);

/* ----------------------------------------------------------------
 * DataChannel API (all profiles)
 * ---------------------------------------------------------------- */

NANO_API int nano_send_datachannel(nano_rtc_t *rtc, uint16_t stream_id, const void *data,
                                   size_t len);
NANO_API int nano_send_datachannel_string(nano_rtc_t *rtc, uint16_t stream_id, const char *str);

/* ----------------------------------------------------------------
 * Media API (conditional)
 * ---------------------------------------------------------------- */

#if NANORTC_PROFILE >= NANO_PROFILE_AUDIO
NANO_API int nano_send_audio(nano_rtc_t *rtc, uint32_t timestamp, const void *data, size_t len);
#endif

#if NANORTC_PROFILE >= NANO_PROFILE_MEDIA
NANO_API int nano_send_video(nano_rtc_t *rtc, uint32_t timestamp, const void *data, size_t len,
                             int is_keyframe);
NANO_API int nano_request_keyframe(nano_rtc_t *rtc);
#endif

#ifdef __cplusplus
}
#endif

#endif /* NANORTC_H_ */
