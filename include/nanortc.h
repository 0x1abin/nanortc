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

/** @brief Mark a symbol as part of the public API (default visibility). */
#if defined(__GNUC__) || defined(__clang__)
#define NANORTC_API __attribute__((visibility("default")))
#else
#define NANORTC_API
#endif

/* ----------------------------------------------------------------
 * Version
 * ---------------------------------------------------------------- */

/** @brief Library version (major.minor.patch). */
/** @{ */
#define NANORTC_VERSION_MAJOR 0
#define NANORTC_VERSION_MINOR 1
#define NANORTC_VERSION_PATCH 0
/** @} */

/* ----------------------------------------------------------------
 * Self-contained byte order (no platform htons/ntohs)
 * ---------------------------------------------------------------- */

/**
 * @brief Convert 16-bit value from host to network byte order.
 * @param x Host-order value.
 * @return Network-order (big-endian) value.
 */
static inline uint16_t nanortc_htons(uint16_t x)
{
    return (uint16_t)((x >> 8) | (x << 8));
}

/**
 * @brief Convert 16-bit value from network to host byte order.
 * @param x Network-order (big-endian) value.
 * @return Host-order value.
 */
static inline uint16_t nanortc_ntohs(uint16_t x)
{
    return nanortc_htons(x);
}

/**
 * @brief Convert 32-bit value from host to network byte order.
 * @param x Host-order value.
 * @return Network-order (big-endian) value.
 */
static inline uint32_t nanortc_htonl(uint32_t x)
{
    return ((x >> 24) & 0x000000FFu) | ((x >> 8) & 0x0000FF00u) | ((x << 8) & 0x00FF0000u) |
           ((x << 24) & 0xFF000000u);
}

/**
 * @brief Convert 32-bit value from network to host byte order.
 * @param x Network-order (big-endian) value.
 * @return Host-order value.
 */
static inline uint32_t nanortc_ntohl(uint32_t x)
{
    return nanortc_htonl(x);
}

/* ----------------------------------------------------------------
 * Safe unaligned big-endian read/write
 *
 * Direct pointer casts (*(uint16_t*)(ptr)) cause HardFault on
 * ARM Cortex-M when ptr is not naturally aligned. These helpers
 * use byte access which compilers optimize to single load/store on
 * platforms that support unaligned access (x86, Cortex-A).
 * ---------------------------------------------------------------- */

#include <string.h>

/**
 * @brief Read a 16-bit big-endian value from an unaligned buffer.
 * @param p Pointer to at least 2 bytes.
 * @return Host-order 16-bit value.
 */
static inline uint16_t nanortc_read_u16be(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

/**
 * @brief Read a 32-bit big-endian value from an unaligned buffer.
 * @param p Pointer to at least 4 bytes.
 * @return Host-order 32-bit value.
 */
static inline uint32_t nanortc_read_u32be(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/**
 * @brief Write a 16-bit value in big-endian format to an unaligned buffer.
 * @param p Pointer to at least 2 bytes of output.
 * @param v Host-order value to write.
 */
static inline void nanortc_write_u16be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v);
}

/**
 * @brief Write a 32-bit value in big-endian format to an unaligned buffer.
 * @param p Pointer to at least 4 bytes of output.
 * @param v Host-order value to write.
 */
static inline void nanortc_write_u32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

/* ----------------------------------------------------------------
 * Error Codes
 * ---------------------------------------------------------------- */

/** @brief Error codes returned by all NanoRTC API functions. */
/** @{ */
#define NANORTC_OK                   0   /**< Success. */
#define NANORTC_ERR_INVALID_PARAM    -1  /**< NULL pointer or out-of-range argument. */
#define NANORTC_ERR_BUFFER_TOO_SMALL -2  /**< Caller-provided buffer is too small. */
#define NANORTC_ERR_STATE            -3  /**< Invalid state for this operation. */
#define NANORTC_ERR_CRYPTO           -4  /**< Cryptographic operation failed. */
#define NANORTC_ERR_PROTOCOL         -5  /**< Protocol violation (remote peer). */
#define NANORTC_ERR_NOT_IMPLEMENTED  -6  /**< Feature not compiled in or not yet implemented. */
#define NANORTC_ERR_PARSE            -7  /**< Malformed input data. */
#define NANORTC_ERR_NO_DATA          -8  /**< No data available (non-fatal). */
#define NANORTC_ERR_INTERNAL         -9  /**< Internal logic error (bug). */
#define NANORTC_ERR_WOULD_BLOCK      -10 /**< Temporary backpressure (send queue full). */
/** @} */

/* Configuration limits are defined in nanortc_config.h */

/* ----------------------------------------------------------------
 * Logging types
 * ---------------------------------------------------------------- */

/** @brief Log severity levels. */
typedef enum {
    NANORTC_LOG_ERROR = 0, /**< Unrecoverable errors that prevent operation. */
    NANORTC_LOG_WARN = 1,  /**< Unusual but recoverable conditions. */
    NANORTC_LOG_INFO = 2,  /**< Normal operation milestones. */
    NANORTC_LOG_DEBUG = 3, /**< Diagnostic information. */
    NANORTC_LOG_TRACE = 4, /**< Detailed packet-level diagnostics. */
} nanortc_log_level_t;

/**
 * @brief Structured log message passed to the user callback.
 *
 * All pointer fields are valid only during the callback invocation.
 * The application must copy any data it needs to retain.
 */
typedef struct nanortc_log_message {
    nanortc_log_level_t level; /**< Severity level. */
    const char *subsystem;     /**< Component tag (e.g. "ICE", "SCTP"). */
    const char *message;       /**< Human-readable message (static string). */
    const char *file;          /**< Source file name, or NULL. */
    uint32_t line;             /**< Source line number, or 0. */
    const char *function;      /**< Function name, or NULL. */
} nanortc_log_message_t;

/**
 * @brief Log callback function type.
 *
 * Called synchronously for each log message at or below the configured
 * level. Must not call NanoRTC functions (no re-entrancy).
 *
 * @param msg   Pointer to the log message (never NULL).
 * @param ctx   User-supplied context pointer from nanortc_log_config_t.
 */
typedef void (*nanortc_log_fn_t)(const nanortc_log_message_t *msg, void *ctx);

/**
 * @brief Logging subsystem configuration.
 *
 * Embed in nanortc_config_t. Set callback to NULL to disable logging.
 * The compile-time NANORTC_LOG_LEVEL caps the runtime level.
 */
typedef struct nanortc_log_config {
    nanortc_log_level_t level; /**< Runtime minimum level (capped by NANORTC_LOG_LEVEL). */
    nanortc_log_fn_t callback; /**< Log callback, or NULL to disable. */
    void *user_data;           /**< Opaque pointer passed to callback. */
} nanortc_log_config_t;

/* ----------------------------------------------------------------
 * Forward declarations
 * ---------------------------------------------------------------- */

#ifndef NANORTC_CRYPTO_PROVIDER_T_DECLARED
#define NANORTC_CRYPTO_PROVIDER_T_DECLARED
typedef struct nanortc_crypto_provider nanortc_crypto_provider_t;
#endif

/* ----------------------------------------------------------------
 * Address type (network-agnostic)
 * ---------------------------------------------------------------- */

/** @brief Network-agnostic socket address (IPv4 / IPv6). */
#ifndef NANORTC_ADDR_T_DECLARED
#define NANORTC_ADDR_T_DECLARED
typedef struct nanortc_addr nanortc_addr_t;
#endif

/* NANORTC_ADDR_SIZE and NANORTC_IPV6_STR_SIZE are in nanortc_config.h */

struct nanortc_addr {
    uint8_t family;                  /**< Address family: 4 = IPv4, 6 = IPv6. */
    uint8_t addr[NANORTC_ADDR_SIZE]; /**< Binary address (network byte order). */
    uint16_t port;                   /**< Port number (host byte order). */
};

/* ----------------------------------------------------------------
 * Output / Event enums
 * ---------------------------------------------------------------- */

/** @brief Type of output produced by nanortc_poll_output(). */
typedef enum {
    NANORTC_OUTPUT_TRANSMIT, /**< UDP data to send to the network. */
    NANORTC_OUTPUT_EVENT,    /**< Application-level event. */
    NANORTC_OUTPUT_TIMEOUT,  /**< Requested callback delay in milliseconds. */
} nanortc_output_type_t;

/** @brief Application event types delivered via NANORTC_OUTPUT_EVENT. */
typedef enum {
    NANORTC_EV_CONNECTED = 0,                 /**< ICE+DTLS(+SCTP) fully established. */
    NANORTC_EV_DISCONNECTED = 1,              /**< Connection lost or closed. */
    NANORTC_EV_ICE_STATE_CHANGE = 2,          /**< ICE state transition. */
    NANORTC_EV_MEDIA_ADDED = 3,               /**< Remote added new media track. */
    NANORTC_EV_MEDIA_CHANGED = 4,             /**< Media direction changed. */
    NANORTC_EV_MEDIA_DATA = 5,                /**< Received media frame (audio or video). */
    NANORTC_EV_KEYFRAME_REQUEST = 6,          /**< Remote requests keyframe (PLI/FIR). */
    NANORTC_EV_DATACHANNEL_OPEN = 7,          /**< DataChannel opened (DCEP complete). */
    NANORTC_EV_DATACHANNEL_DATA = 8,          /**< DataChannel data received. */
    NANORTC_EV_DATACHANNEL_CLOSE = 9,         /**< DataChannel closed. */
    NANORTC_EV_DATACHANNEL_BUFFERED_LOW = 10, /**< Send buffer drained below threshold. */
#if NANORTC_FEATURE_VIDEO
    NANORTC_EV_BITRATE_ESTIMATE = 11, /**< BWE: estimated bitrate changed significantly. */
#endif
    NANORTC_EV_ICE_CANDIDATE = 12, /**< New local ICE candidate discovered (trickle). */
#if NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_RATE_CONTROL
    NANORTC_EV_SPEC_RECOMMENDATION = 13, /**< Adaptive controller recommends a new video spec. */
#endif
} nanortc_event_type_t;

/* Forward declarations needed by event data structures */
typedef struct nanortc nanortc_t;

/* ----------------------------------------------------------------
 * Per-event data structures (str0m-inspired typed events)
 * ---------------------------------------------------------------- */

/** @brief Data for NANORTC_EV_CONNECTED: connection fully established. */
typedef struct {
#if NANORTC_HAVE_MEDIA_TRANSPORT
    uint8_t mids[NANORTC_MAX_MEDIA_TRACKS]; /**< MIDs of sendable media tracks. */
    uint8_t mid_count;                      /**< Number of valid entries in mids[]. */
#else
    uint8_t _pad; /**< Placeholder when media transport is disabled. */
#endif
} nanortc_ev_connected_t;

/** @brief Data for NANORTC_EV_MEDIA_ADDED: remote added a new media track. */
typedef struct {
    uint8_t mid;  /**< Media ID (track index). */
    uint8_t kind; /**< nanortc_track_kind_t: NANORTC_TRACK_AUDIO or NANORTC_TRACK_VIDEO. */
    nanortc_direction_t direction; /**< Negotiated local direction for this track. */
} nanortc_ev_media_added_t;

/** @brief Data for NANORTC_EV_MEDIA_CHANGED: media direction changed. */
typedef struct {
    uint8_t mid;                       /**< Media ID. */
    nanortc_direction_t old_direction; /**< Previous direction. */
    nanortc_direction_t new_direction; /**< New direction. */
} nanortc_ev_media_changed_t;

/** @brief Data for NANORTC_EV_MEDIA_DATA: received media frame (audio or video). */
typedef struct {
    uint8_t mid;         /**< Media ID. */
    uint8_t pt;          /**< RTP payload type. */
    uint32_t timestamp;  /**< RTP timestamp. */
    const uint8_t *data; /**< Depayloaded frame data. */
    size_t len;          /**< Frame data length in bytes. */
    bool contiguous;     /**< True if no gaps since previous frame. */
    bool is_keyframe;    /**< True if video keyframe (always false for audio). */
} nanortc_ev_media_data_t;

/** @brief Data for NANORTC_EV_KEYFRAME_REQUEST. */
typedef struct {
    uint8_t mid; /**< Video track MID that needs a keyframe. */
} nanortc_ev_keyframe_request_t;

#if NANORTC_FEATURE_VIDEO
/** @brief BWE estimate change direction. Lets applications distinguish
 *  "we can push more" from "we must back off" without diffing the bps
 *  value themselves. */
typedef enum {
    NANORTC_BWE_DIR_STABLE = 0, /**< Change is inside the no-event threshold. */
    NANORTC_BWE_DIR_UP = 1,     /**< Bitrate estimate increased. */
    NANORTC_BWE_DIR_DOWN = 2,   /**< Bitrate estimate decreased. */
} nanortc_bwe_direction_t;

/** @brief Which congestion-control signal produced the estimate update. */
typedef enum {
    NANORTC_BWE_SRC_REMB = 0,      /**< REMB feedback (draft-alvestrand-rmcat-remb-03). */
    NANORTC_BWE_SRC_TWCC_LOSS = 1, /**< TWCC loss-based signal (draft-holmer-rmcat-twcc-01). */
} nanortc_bwe_source_t;

/** @brief Data for NANORTC_EV_BITRATE_ESTIMATE: BWE estimate changed.
 *
 *  The @c direction and @c source fields were added in Phase 9. Existing
 *  event consumers that ignore them remain source-compatible; the struct
 *  grows only by appending to the end. */
typedef struct {
    uint32_t bitrate_bps;      /**< Current estimated bitrate (bps). */
    uint32_t prev_bitrate_bps; /**< Previous estimated bitrate (bps). */
    uint8_t direction;         /**< nanortc_bwe_direction_t value. */
    uint8_t source;            /**< nanortc_bwe_source_t value. */
} nanortc_ev_bitrate_estimate_t;
#endif

#if NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_RATE_CONTROL
/** @brief One rung of an adaptive-video capability ladder (Phase 14).
 *
 *  The application supplies an array of these — the encoder/camera geometries
 *  it can actually produce — and the SDK's rate controller recommends which
 *  rung to use for the current bottleneck bandwidth. The array MUST be ordered
 *  ascending by @c bitrate_bps (rung 0 = lowest spec). The library treats the
 *  ladder as caller-owned and read-only; it stores no copy. */
typedef struct {
    uint16_t width;       /**< Frame width in pixels. */
    uint16_t height;      /**< Frame height in pixels. */
    uint8_t fps;          /**< Frame rate (frames/sec). */
    uint32_t bitrate_bps; /**< Target encoder bitrate at this rung (bps). */
} nanortc_spec_rung_t;

/** @brief Data for NANORTC_EV_SPEC_RECOMMENDATION: the adaptive rate controller
 *  recommends switching the video spec to this rung. The library DECIDES; the
 *  application APPLIES it to its encoder (resolution / fps / bitrate). The rung
 *  fields are copied from the application's own ladder for convenience. */
typedef struct {
    uint8_t rung;         /**< Index into the application's capability ladder. */
    uint16_t width;       /**< Recommended frame width (pixels). */
    uint16_t height;      /**< Recommended frame height (pixels). */
    uint8_t fps;          /**< Recommended frame rate (frames/sec). */
    uint32_t bitrate_bps; /**< Recommended encoder bitrate (bps). */
} nanortc_ev_spec_recommendation_t;
#endif

/** @brief Data for NANORTC_EV_ICE_CANDIDATE (trickle ICE). */
typedef struct {
    const char *candidate_str; /**< SDP candidate line (valid until next state mutation). */
    bool end_of_candidates;    /**< True = no more local candidates. */
} nanortc_ev_ice_candidate_t;

/** @brief Data for NANORTC_EV_DATACHANNEL_OPEN. */
typedef struct {
    uint16_t id;       /**< SCTP stream ID. */
    const char *label; /**< Channel label (valid until next state mutation). */
} nanortc_ev_datachannel_open_t;

/** @brief Data for NANORTC_EV_DATACHANNEL_DATA. */
typedef struct {
    uint16_t id;         /**< SCTP stream ID. */
    const uint8_t *data; /**< Payload pointer (valid until next state mutation). */
    size_t len;          /**< Payload length in bytes. */
    bool binary;         /**< true = binary, false = UTF-8 string. */
} nanortc_ev_datachannel_data_t;

/** @brief Data for NANORTC_EV_DATACHANNEL_CLOSE / NANORTC_EV_DATACHANNEL_BUFFERED_LOW. */
typedef struct {
    uint16_t id; /**< SCTP stream ID. */
} nanortc_ev_datachannel_id_t;

/* ----------------------------------------------------------------
 * Event structure (tagged union)
 * ---------------------------------------------------------------- */

/**
 * @brief Application event delivered through nanortc_poll_output().
 *
 * Pointer fields remain valid until the next state-modifying API call on the
 * same instance. Copy them before polling, feeding input, sending, or
 * destroying the instance again.
 */
typedef struct nanortc_event {
    nanortc_event_type_t type; /**< Event type discriminator. */
    union {
        nanortc_ev_connected_t connected;               /**< EV_CONNECTED */
        nanortc_ev_media_added_t media_added;           /**< EV_MEDIA_ADDED */
        nanortc_ev_media_changed_t media_changed;       /**< EV_MEDIA_CHANGED */
        nanortc_ev_media_data_t media_data;             /**< EV_MEDIA_DATA */
        nanortc_ev_keyframe_request_t keyframe_request; /**< EV_KEYFRAME_REQUEST */
#if NANORTC_FEATURE_VIDEO
        nanortc_ev_bitrate_estimate_t bitrate_estimate; /**< EV_BITRATE_ESTIMATE */
#endif
#if NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_RATE_CONTROL
        nanortc_ev_spec_recommendation_t spec_recommendation; /**< EV_SPEC_RECOMMENDATION */
#endif
        nanortc_ev_datachannel_open_t datachannel_open; /**< EV_DATACHANNEL_OPEN */
        nanortc_ev_datachannel_data_t datachannel_data; /**< EV_DATACHANNEL_DATA */
        nanortc_ev_datachannel_id_t
            datachannel_id; /**< EV_DATACHANNEL_CLOSE, EV_DATACHANNEL_BUFFERED_LOW */
        uint16_t ice_state; /**< EV_ICE_STATE_CHANGE */
        nanortc_ev_ice_candidate_t ice_candidate; /**< EV_ICE_CANDIDATE */
    };
} nanortc_event_t;

/* ----------------------------------------------------------------
 * Output structure
 * ---------------------------------------------------------------- */

/**
 * @brief Output item produced by nanortc_poll_output(). Tagged union on @c type.
 *
 * Pointer fields (@c transmit.data, @c event pointer fields) remain valid
 * until the next state-modifying API call on the same @c nanortc_t. This
 * includes another poll, input/timer handling, media/DataChannel send, track
 * or negotiation mutation, and destroy; pure const queries do not invalidate
 * them. Drain each output synchronously, or memcpy() the payload before
 * continuing the event loop.
 */
typedef struct nanortc_output {
    nanortc_output_type_t type; /**< Discriminator for the anonymous union. */
    union {
        struct {
            const uint8_t *data; /**< Packet payload. */
            size_t len;          /**< Packet length in bytes. */
            nanortc_addr_t dest; /**< Destination address. */
            nanortc_addr_t src;  /**< Source address (local interface).
                                  *   family==0 means "use any" (backward compat). */
        } transmit;              /**< Valid when type == NANORTC_OUTPUT_TRANSMIT. */
        nanortc_event_t event;   /**< Valid when type == NANORTC_OUTPUT_EVENT. */
        uint32_t timeout_ms;     /**< Valid when type == NANORTC_OUTPUT_TIMEOUT. */
    };
} nanortc_output_t;

/* ----------------------------------------------------------------
 * Input structure
 * ---------------------------------------------------------------- */

/**
 * @brief Network input passed to nanortc_handle_input().
 *
 * Mirrors the shape of nanortc_output_t.transmit so callers can think of
 * each side symmetrically: incoming carry @c (now_ms, src, dst, data, len);
 * outgoing transmits carry @c (src, dest, data, len). The @c now_ms
 * timestamp is part of the input the same way str0m's @c Input::Timeout
 * and @c Input::Receive both carry an @c Instant.
 *
 * For a pure timer tick (no packet to deliver), set @c data=NULL,
 * @c len=0, and @c src.family=0 — only @c now_ms is required:
 *
 *   nanortc_input_t tick = { .now_ms = now };
 *   nanortc_handle_input(rtc, &tick);
 *
 * When a packet is delivered, @c family==0 on the address fields means
 * unset:
 *   - @c src.family==0 disables packet processing (treated as timer-only).
 *   - @c dst.family==0 falls back to the legacy "selected_local_idx=0"
 *     behaviour, correct on single-host-candidate setups. See
 *     rtc_resolve_local_idx() in src/nano_rtc.c for the matching rules.
 */
typedef struct nanortc_input {
    uint32_t now_ms;     /**< Current monotonic time in milliseconds. */
    const uint8_t *data; /**< Packet payload. NULL when only ticking timers. */
    size_t len;          /**< Packet length in bytes. 0 if no packet. */
    nanortc_addr_t src;  /**< Remote source address. family==0 means unset. */
    nanortc_addr_t dst;  /**< Local interface that received the packet.
                          *   family==0 means unknown — ICE then falls back
                          *   to selected_local_idx=0 (legacy behaviour). */
} nanortc_input_t;

/* Handle types are defined after nanortc_t (forward declaration needed) */

/* nanortc_direction_t is defined in nanortc_config.h */

/* ----------------------------------------------------------------
 * Codec identifiers
 * ---------------------------------------------------------------- */

/** @brief Supported audio/video codec identifiers for SDP negotiation. */
typedef enum {
    NANORTC_CODEC_NONE = 0, /**< No codec selected. */
    NANORTC_CODEC_OPUS,     /**< Opus audio (RFC 6716). */
    NANORTC_CODEC_PCMA,     /**< G.711 A-law (RFC 3551). */
    NANORTC_CODEC_PCMU,     /**< G.711 mu-law (RFC 3551). */
    NANORTC_CODEC_H264,     /**< H.264 video (RFC 6184). */
    NANORTC_CODEC_VP8,      /**< VP8 video (RFC 7741). */
    NANORTC_CODEC_H265,     /**< H.265/HEVC video (RFC 7798). */
} nanortc_codec_t;

/* ----------------------------------------------------------------
 * ICE role
 * ---------------------------------------------------------------- */

/** @brief ICE agent role (RFC 8445). */
typedef enum {
    NANORTC_ROLE_CONTROLLED,  /**< Answerer: respond to STUN checks (ICE-Lite). */
    NANORTC_ROLE_CONTROLLING, /**< Offerer: initiate STUN connectivity checks. */
} nanortc_ice_role_t;

/* ----------------------------------------------------------------
 * ICE Server Configuration (WebRTC RTCIceServer)
 * ---------------------------------------------------------------- */

/**
 * @brief ICE server descriptor (matches WebRTC RTCIceServer).
 *
 * Each entry may contain one or more URLs. The URL prefix determines
 * the type: "stun:host:port" or "turn:host:port".
 *
 * Supports both single-URL and multi-URL forms:
 *   - Single: {.urls = (const char*[]){"stun:host:3478"}, .url_count = 1}
 *   - Multi:  {.urls = (const char*[]){"turn:h:3478", "turn:h:3478?transport=tcp"},
 *              .url_count = 2, .username = "user", .credential = "pass"}
 *
 * Pointer fields must remain valid only during the nanortc_init() call
 * (contents are copied internally).
 */
typedef struct nanortc_ice_server {
    const char *const *urls; /**< Array of server URLs ("stun:..." or "turn:..."). */
    size_t url_count;        /**< Number of entries in urls[]. */
    const char *username;    /**< TURN username (NULL for STUN). */
    const char *credential;  /**< TURN credential/password (NULL for STUN). */
} nanortc_ice_server_t;

/* ----------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------- */

/** @brief Configuration passed to nanortc_init(). */
typedef struct nanortc_config {
    const nanortc_crypto_provider_t *crypto; /**< Crypto backend (required, non-NULL). */
    nanortc_ice_role_t role;                 /**< ICE role (controlled or controlling). */

    /** @brief Logging configuration (optional, zero-init disables). */
    nanortc_log_config_t log;

    /** @brief ICE servers (STUN/TURN). Matches WebRTC RTCConfiguration.iceServers.
     *  Pointer fields are copied during nanortc_init(); need not persist after. */
    const nanortc_ice_server_t *ice_servers; /**< Array of ICE server descriptors (NULL = none). */
    size_t ice_server_count;                 /**< Number of entries in ice_servers[]. */

    /* Memory configuration */
    uint32_t sctp_send_buf_size; /**< SCTP send buffer size (0 = default). */
    uint32_t sctp_recv_buf_size; /**< SCTP receive buffer size (0 = default). */

#if NANORTC_FEATURE_AUDIO
    uint32_t jitter_depth_ms; /**< Jitter buffer depth in ms (default for new audio tracks). */
#endif
} nanortc_config_t;

/** @brief Default-initialize a nanortc_config_t. Caller must set .crypto before nanortc_init(). */
#define NANORTC_CONFIG_DEFAULT()                        \
    (nanortc_config_t)                                  \
    {                                                   \
        .crypto = NULL, .role = NANORTC_ROLE_CONTROLLED \
    }

/* ----------------------------------------------------------------
 * Internal subsystem types (needed for struct layout — do not use directly)
 * ---------------------------------------------------------------- */
#include "nano_ice.h"
#include "nano_dtls.h"
#include "nano_sdp.h"
#if NANORTC_FEATURE_TURN
#include "nano_turn.h"
#endif

#if NANORTC_FEATURE_DATACHANNEL
#include "nano_sctp.h"
#include "nano_datachannel.h"
#endif

#if NANORTC_HAVE_MEDIA_TRANSPORT
#include "nano_media.h"
#include "nano_srtp.h"
#endif

#if NANORTC_FEATURE_VIDEO
#include "nano_bwe.h"
#include "nano_rate_control.h"
#endif

#if NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_PACING
/**
 * @brief Send-side video RTP pacer (leaky token bucket).
 *
 * Meters outbound video fragments out of the pkt_ring into the output
 * queue at the BWE-derived rate so a multi-fragment IDR is spread across
 * up to one frame interval instead of bursting onto the wire. The FIFO is
 * a [head, tail) window over the same pkt_ring slots the NACK history
 * uses; `tail` advances in lock-step with `pkt_ring_tail` (every committed
 * fragment is pace-enqueued), `head` is the release cursor. Logic lives in
 * src/nano_rtc_media.c (pacer_enqueue / pacer_pump). @internal
 */
typedef struct {
    uint16_t head;            /**< Release cursor: next pkt_ring slot to pump. */
    uint16_t tail;            /**< Enqueue cursor: == pkt_ring_tail. depth = tail-head. */
    uint32_t budget_bytes;    /**< Token-bucket credit available to release now. */
    uint32_t last_refill_ms;  /**< Timestamp of the last token-bucket refill. */
    bool refill_inited;       /**< last_refill_ms is valid, including timestamp zero. */
    uint32_t next_release_ms; /**< Cached deadline for nanortc_next_timeout_ms(). */
    /** Per-slot enqueue time, indexed by (cursor & (PKT_RING_SIZE-1)); used to
     *  cap the latency the pacer may add (NANORTC_PACING_MAX_QUEUE_MS catch-up). */
    uint32_t enqueue_ms[NANORTC_VIDEO_PKT_RING_SIZE];
} nano_pacer_t;
#endif

/* ----------------------------------------------------------------
 * Connection state
 * ---------------------------------------------------------------- */

typedef enum {
    NANORTC_STATE_NEW,
    NANORTC_STATE_ICE_CHECKING,
    NANORTC_STATE_ICE_CONNECTED,
    NANORTC_STATE_DTLS_HANDSHAKING,
    NANORTC_STATE_DTLS_CONNECTED,
    NANORTC_STATE_SCTP_CONNECTING,
    NANORTC_STATE_CONNECTED,
    NANORTC_STATE_CLOSED,
} nano_conn_state_t;

/* ----------------------------------------------------------------
 * Main state machine
 * ---------------------------------------------------------------- */

/**
 * @brief RTC state machine.
 *
 * @internal Layout is public for stack/static allocation only.
 *           Do not access struct members directly — use the nanortc_*() API.
 *           Internal layout may change between releases.
 */
struct nanortc {
    nanortc_config_t config;
    nano_conn_state_t state;
    uint32_t now_ms; /* last known time */

    /* Subsystem state */
    nano_ice_t ice;
    nano_dtls_t dtls;
    nano_sdp_t sdp;
#if NANORTC_FEATURE_TURN
    nano_turn_t turn;
#endif

#if NANORTC_FEATURE_DATACHANNEL
    nano_sctp_t sctp;
    nano_dc_t datachannel;
#endif

#if NANORTC_HAVE_MEDIA_TRANSPORT
    /** Media tracks (str0m-inspired: indexed by MID). */
    nanortc_track_t media[NANORTC_MAX_MEDIA_TRACKS];
    uint8_t media_count; /**< Number of allocated media track slots. */

    /** SSRC → MID lookup table for RTP receive-path demuxing. */
    nanortc_ssrc_entry_t ssrc_map[NANORTC_MAX_SSRC_MAP];

    /** Shared SRTP session (keys shared across all tracks in BUNDLE). */
    nano_srtp_t srtp;

    /** Last time RTCP SR was sent (for periodic RTCP, RFC 3550 §6.2). */
    uint32_t last_rtcp_send_ms;
    bool last_rtcp_send_valid; /**< True once last_rtcp_send_ms has been committed. */

    /** Round-robin cursor for the multi-track SR cadence. */
    uint8_t sr_cursor;
#endif

#if NANORTC_FEATURE_VIDEO
    /** NACK retransmit ring: each slot holds one SRTP-protected video packet.
     *  Sized by NANORTC_VIDEO_PKT_RING_SIZE (independent of OUT_QUEUE_SIZE)
     *  so IoT targets can shrink the NACK window without starving the output
     *  dispatch queue. See nanortc_config.h for the slot-reuse invariant. */
    uint8_t pkt_ring[NANORTC_VIDEO_PKT_RING_SIZE][NANORTC_MEDIA_BUF_SIZE];

    /** NACK retransmission metadata — tracks which RTP seq lives in each
     *  pkt_ring slot so we can retransmit on RTCP NACK (RFC 4585 §6.2.1).
     *  len==0 means the slot is empty/invalid. */
    struct {
        uint16_t seq; /**< RTP sequence number stored in this slot. */
        uint16_t len; /**< SRTP-protected packet length (0 = invalid). */
    } pkt_ring_meta[NANORTC_VIDEO_PKT_RING_SIZE];

    /** Independent write cursor for pkt_ring (decoupled from out_tail). */
    uint16_t pkt_ring_tail;

    /** NACK-answer retransmit scratch ring (TD-023 fix). A generic-NACK
     *  retransmit copies the matched pkt_ring packet HERE and enqueues this copy,
     *  so a concurrent `nanortc_send_video()` that wraps pkt_ring over the
     *  original slot cannot corrupt the in-flight retransmit. Each slot tracks
     *  its own `free_at` (out_head value at which its prior output dequeued),
     *  identical to the FEC tx ring guard. Bursts beyond the ring fall back to
     *  best-effort (re-NACKable). Bounded RAM: NANORTC_NACK_RETX_RING × MEDIA_BUF. */
    uint8_t nack_retx_buf[NANORTC_NACK_RETX_RING][NANORTC_MEDIA_BUF_SIZE];
    uint16_t nack_retx_free_at[NANORTC_NACK_RETX_RING];
    uint64_t nack_retx_in_use; /**< Exact lifetime bits, cleared when out_head reaches free_at. */

    /** Count of times video_send_fragment_cb wrapped pkt_ring while a
     *  prior slot was still referenced by an outstanding out_queue entry
     *  (NANORTC_VIDEO_PKT_RING_SIZE under-sized vs the per-frame fragment
     *  count). Each increment also emits NANORTC_LOGW. Read this from
     *  integration smoke tests to catch under-sizing before it reaches
     *  the wire — see docs/engineering/memory-profiles.md. */
    uint32_t stats_pkt_ring_overrun;

    /** Shared bandwidth estimator (session-wide, not per-track). */
    nano_bwe_t bwe;

#if NANORTC_FEATURE_VIDEO_RATE_CONTROL
    /** Adaptive media spec controller (session-wide). Pure-compute: maps the
     *  BWE estimate + smoothed loss to a recommended capability-ladder rung. */
    nano_rate_control_t rc;
    /** Caller-owned capability ladder set via nanortc_set_capability_ladder().
     *  Stored by reference only (zero-copy); the caller must keep the array
     *  alive for the session. NULL / rc_ladder_n == 0 means the controller is
     *  idle and emits no NANORTC_EV_SPEC_RECOMMENDATION. */
    const nanortc_spec_rung_t *rc_ladder;
    uint8_t rc_ladder_n;
#endif

#if NANORTC_FEATURE_VIDEO_PACING
    /** Send-side video RTP pacer (BWE-driven leaky token bucket). Meters
     *  video fragments out of pkt_ring into out_queue so an IDR burst does
     *  not overrun the network bottleneck. See src/nano_rtc_media.c. */
    nano_pacer_t pacer;

    /** Video fragments released to the output queue by the pacer. */
    uint32_t stats_paced_packets;
    /** Pacer catch-up drains: times the oldest queued fragment hit
     *  NANORTC_PACING_MAX_QUEUE_MS and the backlog was flushed immediately
     *  to bound added latency. A rising count means the link is slower than
     *  the encoder's output (encoder should drop bitrate). */
    uint32_t stats_pace_catchup;
#endif
#if NANORTC_FEATURE_VIDEO_AUTO_PLI
    /** Auto-PLI keyframe requests emitted on detected receive-side loss
     *  (forward RTP sequence gap). A rising count tracks inbound video loss. */
    uint32_t stats_auto_pli_sent;
#endif
#if NANORTC_FEATURE_VIDEO_NACK_RX
    /** Receiver-generated NACKs (Generic NACK feedback for lost packets). */
    uint32_t stats_nack_sent;
#endif
#if NANORTC_FEATURE_VIDEO_FEC
    /** ULPFEC (Phase 13 PR-3/4). Single-video-track scope: state is rtc-level
     *  (one media stream). The FEC stream rides media SSRC + 1 on PT
     *  NANORTC_VIDEO_FEC_PT. */
    /* Send: rolling group of the last K plaintext media RTP packets → one FEC
     * packet per completed group. */
    uint8_t fec_tx_grp[NANORTC_FEC_GROUP_SIZE][NANORTC_MEDIA_BUF_SIZE];
    uint16_t fec_tx_len[NANORTC_FEC_GROUP_SIZE];
    uint8_t fec_tx_n;    /**< Members buffered in the current group. */
    uint16_t fec_tx_seq; /**< FEC stream sequence counter. */
    /** Ring of FEC RTP packet scratch buffers — one per in-flight FEC. A bursty
     *  frame (large IDR) completes several FEC groups in one send call; each
     *  group's FEC needs its own buffer so the pointers handed to out_queue do
     *  not alias. `fec_tx_free_at[i]` is the out_head value at which slot i's
     *  prior FEC output has been dequeued and the slot is reusable. */
    uint8_t fec_tx_buf[NANORTC_FEC_TX_RING][NANORTC_FEC_BUF_SIZE];
    uint16_t fec_tx_free_at[NANORTC_FEC_TX_RING];
    uint64_t fec_tx_in_use; /**< Exact lifetime bits, cleared at the matching dequeue cursor. */
    /* Receive: ring of recent plaintext media packets for FEC recovery. */
    uint8_t fec_rx_med[NANORTC_FEC_GROUP_SIZE][NANORTC_MEDIA_BUF_SIZE];
    uint16_t fec_rx_len[NANORTC_FEC_GROUP_SIZE];
    uint8_t fec_rx_n;                             /**< Ring write cursor. */
    uint8_t fec_rx_recov[NANORTC_MEDIA_BUF_SIZE]; /**< Recovered packet scratch. */
    /** One FEC packet awaiting its group's late media (FEC delivered before
     *  enough members arrived); retried as each media packet is buffered so
     *  recovery is independent of FEC-vs-media wire ordering. 0 = none. */
    uint8_t fec_rx_pending[NANORTC_FEC_BUF_SIZE];
    uint16_t fec_rx_pending_len;
    /** The most recent received FEC's protected SN window (RFC 5109 level-0: SN
     *  base + 16-bit mask). Lets the receiver NACK skip a packet the FEC will
     *  recover — FEC and NACK are complementary recovery, not duplicate requests
     *  for the same loss (RFC 5109 §10.1 + RFC 4585 §6.2.1). Set when a FEC
     *  packet arrives, which (by nanortc's send order) precedes the media gap it
     *  covers. */
    uint16_t fec_prot_base;
    uint16_t fec_prot_mask;
    bool fec_prot_valid;
    uint32_t stats_fec_sent;             /**< FEC packets emitted. */
    uint32_t stats_fec_dropped_resource; /**< FEC groups skipped to preserve media admission. */
    uint32_t stats_fec_recovered;        /**< Media packets recovered via FEC. */
    uint32_t stats_nack_suppressed_fec;  /**< NACK events skipped because FEC covers the loss. */
#endif
#endif

    /* Output queue (simple ring buffer) */
    nanortc_output_t out_queue[NANORTC_OUT_QUEUE_SIZE];
    uint16_t out_head;
    uint16_t out_tail;

    /** Owned backing storage for transient transmit producers. A slot remains
     * busy until poll_output advances out_head to its exact free_at cursor. */
    uint8_t tx_slots[NANORTC_TX_SLOT_COUNT][NANORTC_TX_SLOT_SIZE];
    uint16_t tx_slot_free_at[NANORTC_TX_SLOT_COUNT];
    uint32_t tx_slots_in_use;
    uint8_t tx_slot_cursor;

#if NANORTC_FEATURE_TURN
    /* Per-output side-table for lazy TURN wrap (RFC 5766 §10/§11). When set,
     * nanortc_poll_output() wraps out_queue[slot].transmit.data into turn_buf
     * (ChannelData if a channel is bound, otherwise Send indication) just
     * before handing the output back to the caller. Lazy wrap is used because
     * the output queue stores only a pointer per slot — eagerly wrapping into
     * a shared scratch at enqueue time would let later enqueues clobber the
     * pending wraps from earlier callers in the same tick (e.g. video FU-A
     * fragments overwriting each other). */
    struct {
        bool via_turn;            /**< Wrap data through TURN at dispatch time. */
        nanortc_addr_t peer_dest; /**< Original peer destination (pre-wrap). */
    } out_wrap_meta[NANORTC_OUT_QUEUE_SIZE];

    /* Diagnostic counters for the TX dispatch path — readable directly by
     * application glue (e.g., an application-layer broadcast helper) to
     * figure out why media packets are or aren't flowing over the relay. */
    uint32_t stats_enqueue_direct;   /**< Enqueues that bypass TURN wrap. */
    uint32_t stats_enqueue_via_turn; /**< Enqueues flagged for TURN wrap. */
    uint32_t stats_wrap_dropped;     /**< Lazy wrap failures in poll_output. */
#endif

    /* PR-2 audit signal — universal across feature combos: incremented when
     * rtc_enqueue_transmit() finds out_queue full and drops the slot. Fires
     * on CORE_ONLY/DATA/AUDIO builds too, not just TURN. */
    uint32_t stats_tx_queue_full; /**< rtc_enqueue_transmit out_queue overflow. */

    /* State-mutating calls invalidate prior output pointers, so receive-side
     * STUN/SRTP scratch and poll-time TURN wrapping can share storage. Their
     * uses never overlap within one API call; outbound control uses TX slots. */
    union {
        uint8_t stun_buf[NANORTC_STUN_BUF_SIZE];
#if NANORTC_FEATURE_TURN
        uint8_t turn_buf[NANORTC_TURN_BUF_SIZE];
#endif
    };

    /* Stored remote address for SCTP output routing */
    nanortc_addr_t remote_addr;

    /* Candidate events are emitted by separate state-mutating API calls, so
     * their strings follow the same pointer-lifetime contract and share one
     * fixed scratch region. */
    union {
#if NANORTC_FEATURE_TURN
        char relay_cand_str[NANORTC_IPV6_STR_SIZE + 96];
#endif
        char srflx_cand_str[NANORTC_IPV6_STR_SIZE + 96];
        char host_cand_str[NANORTC_IPV6_STR_SIZE + 96];
    };

    /* STUN server for srflx discovery (RFC 8445 §5.1.1.1) */
    uint8_t stun_server_addr[NANORTC_ADDR_SIZE];
    uint16_t stun_server_port;
    uint8_t stun_server_family; /* 4 or 6 */
    bool stun_server_configured;
    uint8_t stun_txid[NANORTC_STUN_TXID_SIZE]; /* transaction ID for srflx request */
    bool srflx_discovered;
    uint32_t stun_retry_at_ms;
    uint8_t stun_retries;
};

/* ----------------------------------------------------------------
 * Lifecycle API
 * ---------------------------------------------------------------- */

/**
 * @brief Initialize the RTC state machine.
 *
 * @param rtc  Caller-allocated state (must be zeroed before first call).
 * @param cfg  Configuration (pointer contents copied; need not persist).
 * @return NANORTC_OK on success.
 * @retval NANORTC_ERR_INVALID_PARAM  @p rtc or @p cfg is NULL, or crypto missing.
 * @retval NANORTC_ERR_CRYPTO         Initial ICE tie-breaker RNG failed.
 */
NANORTC_API int nanortc_init(nanortc_t *rtc, const nanortc_config_t *cfg);

/**
 * @brief Release resources held by the RTC state machine.
 * @param rtc  State previously initialized with nanortc_init(), or NULL (no-op).
 */
NANORTC_API void nanortc_destroy(nanortc_t *rtc);

/* ----------------------------------------------------------------
 * SDP API
 * ---------------------------------------------------------------- */

/**
 * @brief Parse a remote SDP offer and generate an SDP answer.
 *
 * @param rtc            Initialized RTC state.
 * @param offer          NUL-terminated remote SDP offer string.
 * @param answer_buf     Buffer to receive the generated SDP answer.
 * @param answer_buf_len Size of @p answer_buf in bytes.
 * @param out_len        Receives answer length in bytes (may be NULL).
 * @return NANORTC_OK on success.
 * @retval NANORTC_ERR_BUFFER_TOO_SMALL  @p answer_buf_len is insufficient.
 * @retval NANORTC_ERR_PARSE             Malformed SDP offer.
 * @retval NANORTC_ERR_STATE             Called on a non-fresh instance; only
 *                                       valid in state NEW (or after
 *                                       nanortc_ice_restart()).
 */
NANORTC_API int nanortc_accept_offer(nanortc_t *rtc, const char *offer, char *answer_buf,
                                     size_t answer_buf_len, size_t *out_len);

/**
 * @brief Generate an SDP offer (controlling/offerer role).
 *
 * @param rtc           Initialized RTC state.
 * @param offer_buf     Buffer to receive the SDP offer.
 * @param offer_buf_len Size of @p offer_buf in bytes.
 * @param out_len       Receives offer length in bytes (may be NULL).
 * @return NANORTC_OK on success.
 * @retval NANORTC_ERR_BUFFER_TOO_SMALL  @p offer_buf_len is insufficient.
 */
NANORTC_API int nanortc_create_offer(nanortc_t *rtc, char *offer_buf, size_t offer_buf_len,
                                     size_t *out_len);

/**
 * @brief Parse a remote SDP answer (after creating an offer).
 *
 * @param rtc     Initialized RTC state (must have called nanortc_create_offer first).
 * @param answer  NUL-terminated remote SDP answer string.
 * @return NANORTC_OK on success.
 * @retval NANORTC_ERR_PARSE  Malformed SDP answer.
 * @retval NANORTC_ERR_STATE  No pending offer.
 */
NANORTC_API int nanortc_accept_answer(nanortc_t *rtc, const char *answer);

/* ----------------------------------------------------------------
 * ICE API
 * ---------------------------------------------------------------- */

/**
 * @brief Add a local ICE candidate (host address).
 *
 * @param rtc   Initialized RTC state.
 * @param ip    NUL-terminated IP address string (IPv4 or IPv6).
 * @param port  UDP port number (host byte order).
 * @return NANORTC_OK on success.
 * @retval NANORTC_ERR_INVALID_PARAM  @p ip is NULL or invalid.
 * @retval NANORTC_ERR_BUFFER_TOO_SMALL  Candidate table full.
 */
NANORTC_API int nanortc_add_local_candidate(nanortc_t *rtc, const char *ip, uint16_t port);

/**
 * @brief Add a remote ICE candidate from an SDP candidate attribute.
 *
 * Can be called at any time (trickle ICE, RFC 8838). If ICE is already in
 * CHECKING state, the new candidate is immediately included in checks.
 *
 * @param rtc            Initialized RTC state.
 * @param candidate_str  NUL-terminated SDP candidate line (a=candidate:...).
 * @return NANORTC_OK on success.
 * @retval NANORTC_ERR_PARSE  Malformed candidate string.
 */
NANORTC_API int nanortc_add_remote_candidate(nanortc_t *rtc, const char *candidate_str);

/**
 * @brief Set ICE servers after initialization.
 *
 * Alternative to nanortc_config_t.ice_servers for cases where ICE server
 * info is obtained after init (e.g., from signaling server response).
 * Call before nanortc_create_offer() / nanortc_accept_offer().
 *
 * @param rtc      Initialized RTC state.
 * @param servers  Array of ICE server descriptors (copied internally).
 * @param count    Number of entries in @p servers.
 * @return NANORTC_OK on success.
 */
NANORTC_API int nanortc_set_ice_servers(nanortc_t *rtc, const nanortc_ice_server_t *servers,
                                        size_t count);

/**
 * @brief Signal end of remote ICE candidates (RFC 8838).
 *
 * After this call, no more candidates will be accepted. If ICE is checking
 * and all candidates have been exhausted, ICE transitions to FAILED.
 *
 * @param rtc  Initialized RTC state.
 * @return NANORTC_OK on success.
 */
NANORTC_API int nanortc_end_of_candidates(nanortc_t *rtc);

/**
 * @brief Trigger an ICE restart (RFC 8445 §9).
 *
 * Resets ICE state, generates new credentials, and clears remote candidates.
 * After calling this, exchange a new offer/answer with the updated credentials.
 *
 * @param rtc  Initialized RTC state.
 * @return NANORTC_OK on success.
 */
NANORTC_API int nanortc_ice_restart(nanortc_t *rtc);

/* ----------------------------------------------------------------
 * Event loop (Sans I/O core)
 * ---------------------------------------------------------------- */

/**
 * @brief Dequeue the next output from the state machine.
 *
 * Call in a loop until it returns NANORTC_ERR_NO_DATA.
 *
 * @param rtc  Initialized RTC state.
 * @param out  Receives the next output item.
 * @return NANORTC_OK if @p out was filled.
 * @retval NANORTC_ERR_NO_DATA  No more pending outputs.
 */
NANORTC_API int nanortc_poll_output(nanortc_t *rtc, nanortc_output_t *out);

/**
 * @brief Feed input into the state machine (unified entry point).
 *
 * Handles both incoming UDP packets and timer advancement in a single call.
 * Always processes pending timers (ICE checks, SCTP retransmits) using
 * @c in->now_ms. If @c in carries packet data, also demuxes and processes
 * the incoming packet (RFC 7983).
 *
 * The struct shape mirrors @c nanortc_output_t.transmit (see
 * #nanortc_input_t). For a pure timer tick set @c data=NULL — only
 * @c now_ms is required.
 *
 * @note For correct timing-sensitive behaviour (DTLS handshake retransmit,
 *       ICE connectivity checks, SCTP retransmit, consent freshness), this
 *       function should be called at least every
 *       @c NANORTC_MIN_POLL_INTERVAL_MS (default 50 ms) even when no packet
 *       is pending — pass an input with @c data=NULL to tick timers only.
 *       Slower polling rates may miss DTLS retransmit deadlines and delay
 *       handshake completion. Use @ref nanortc_next_timeout_ms to drive an
 *       event loop that blocks in select()/poll()/epoll_wait() up to the
 *       library's actual next deadline instead of fixed periodic ticks.
 *
 * @param rtc Initialized RTC state.
 * @param in  Input bundle (must not be NULL). See #nanortc_input_t.
 * @return NANORTC_OK on success.
 */
NANORTC_API int nanortc_handle_input(nanortc_t *rtc, const nanortc_input_t *in);

/**
 * @brief Compute milliseconds until the next protocol deadline.
 *
 * Lets event-loop callers replace fixed periodic @ref nanortc_handle_input
 * ticks with select() / poll() / epoll_wait() blocking up to the returned
 * delay. After the wait elapses (or earlier if a socket signals input),
 * call @ref nanortc_handle_input with the current @c now_ms.
 *
 * Aggregates ICE connectivity-check pacing, ICE consent freshness send +
 * expiry, STUN srflx retry, TURN Allocate / Refresh / CreatePermission /
 * ChannelBind refresh, SCTP retransmission RTOs and heartbeat, and the
 * RTCP Sender Report period. While a DTLS handshake is in progress the
 * result is capped at @c NANORTC_MIN_POLL_INTERVAL_MS so the underlying
 * crypto provider's retransmits still fire on time. When no deadline is
 * armed, the function returns a conservative idle cap (1000 ms) so the
 * caller never sleeps indefinitely.
 *
 * Pure const reader — does not mutate @p rtc and may be called from any
 * thread that already owns the @c nanortc_t under the usual single-owner
 * discipline. Cheap (a handful of comparisons), so calling it on every
 * iteration of the event loop is fine.
 *
 * @param[in]  rtc     Initialized RTC state.
 * @param[in]  now_ms  Current monotonic time, same clock as elsewhere.
 * @param[out] out_ms  Receives the maximum recommended sleep, in ms.
 *                     0 means "tick immediately".
 *
 * @retval NANORTC_OK              Always when params are valid.
 * @retval NANORTC_ERR_INVALID_PARAM @p rtc or @p out_ms is NULL.
 */
NANORTC_API int nanortc_next_timeout_ms(const nanortc_t *rtc, uint32_t now_ms, uint32_t *out_ms);

/**
 * @brief Number of free slots in the output queue.
 *
 * Each pending TRANSMIT / EVENT output occupies one slot until drained by
 * @ref nanortc_poll_output. Senders that emit multi-packet bursts (one
 * video frame fragments into ceil(len / NANORTC_VIDEO_MTU) RTP packets)
 * can query this before sending and drain the queue first instead of
 * overflowing it mid-frame.
 *
 * Pure const reader — same single-owner threading discipline as
 * @ref nanortc_next_timeout_ms.
 *
 * This reports only the universal output queue. It does not include the
 * shared transient TX-slot ring, video packet/pacer storage, FEC/NACK rings,
 * or protocol-specific pending capacity; a send may therefore still return
 * @c NANORTC_ERR_WOULD_BLOCK when this value is non-zero.
 *
 * @param rtc  Initialized RTC state.
 * @return Free slot count (0..NANORTC_OUT_QUEUE_SIZE); 0 if @p rtc is NULL.
 */
NANORTC_API uint16_t nanortc_output_free_slots(const nanortc_t *rtc);

/* ----------------------------------------------------------------
 * DataChannel types
 * ---------------------------------------------------------------- */

#if NANORTC_FEATURE_DATACHANNEL

/** @brief Optional DataChannel parameters for nanortc_create_datachannel().
 *  Pass NULL for defaults (reliable, ordered). Zero-initialized struct also gives defaults. */
typedef struct nanortc_datachannel_options {
    const char *protocol;     /**< Sub-protocol (NUL-terminated, NULL = none). */
    bool unordered;           /**< Set true for unordered delivery (default: false = ordered). */
    uint16_t max_retransmits; /**< Max retransmit count (0 = reliable). */
} nanortc_datachannel_options_t;

#endif

/* ----------------------------------------------------------------
 * Media API (multi-track, str0m-inspired)
 * ---------------------------------------------------------------- */

#if NANORTC_HAVE_MEDIA_TRANSPORT

/**
 * @brief Add an audio track to the SDP session.
 *
 * Call before nanortc_create_offer() or nanortc_accept_offer().
 * Returns the MID (media ID) which is the track handle.
 *
 * @param rtc         Initialized RTC state.
 * @param direction   Send/receive direction for this track.
 * @param codec       Codec to negotiate (e.g. NANORTC_CODEC_OPUS).
 * @param sample_rate Sample rate in Hz (e.g. 48000 for Opus).
 * @param channels    Audio channels (1 = mono, 2 = stereo).
 * @return MID (>= 0) on success, negative error code on failure.
 */
NANORTC_API int nanortc_add_audio_track(nanortc_t *rtc, nanortc_direction_t direction,
                                        nanortc_codec_t codec, uint32_t sample_rate,
                                        uint8_t channels);

/**
 * @brief Add a video track to the SDP session.
 *
 * Call before nanortc_create_offer() or nanortc_accept_offer().
 * Returns the MID (media ID) which is the track handle.
 *
 * @param rtc       Initialized RTC state.
 * @param direction Send/receive direction for this track.
 * @param codec     Codec to negotiate (e.g. NANORTC_CODEC_H264).
 * @return MID (>= 0) on success, negative error code on failure.
 */
NANORTC_API int nanortc_add_video_track(nanortc_t *rtc, nanortc_direction_t direction,
                                        nanortc_codec_t codec);

#if NANORTC_FEATURE_H265
/**
 * @brief Provide the out-of-band VPS/SPS/PPS NAL units for an H.265 track.
 *
 * Must be called after nanortc_add_video_track() with NANORTC_CODEC_H265 and
 * before nanortc_create_offer() / nanortc_accept_offer(). The library
 * base64-encodes the three parameter-set NALs (RFC 4648) and emits them as
 * sprop-vps / sprop-sps / sprop-pps fmtp parameters (RFC 7798 §7.1).
 *
 * Each NAL must be the raw NAL unit (2-byte H.265 NAL header + RBSP), with
 * no Annex-B start code.
 *
 * @param rtc      Initialized RTC state with an H.265 video track added.
 * @param mid      Track MID returned by nanortc_add_video_track().
 * @param vps      VPS NAL bytes.
 * @param vps_len  Length of @p vps in bytes.
 * @param sps      SPS NAL bytes.
 * @param sps_len  Length of @p sps in bytes.
 * @param pps      PPS NAL bytes.
 * @param pps_len  Length of @p pps in bytes.
 * @return NANORTC_OK on success, negative error code otherwise.
 */
NANORTC_API int nanortc_video_set_h265_parameter_sets(nanortc_t *rtc, uint8_t mid,
                                                      const uint8_t *vps, size_t vps_len,
                                                      const uint8_t *sps, size_t sps_len,
                                                      const uint8_t *pps, size_t pps_len);
#endif /* NANORTC_FEATURE_H265 */

/**
 * @brief Change direction of an existing media track.
 *
 * @param rtc  Initialized RTC state.
 * @param mid  Track MID returned by nanortc_add_audio_track() / nanortc_add_video_track().
 * @param dir  New direction.
 */
NANORTC_API void nanortc_set_direction(nanortc_t *rtc, uint8_t mid, nanortc_direction_t dir);

/* ----------------------------------------------------------------
 * Media send API
 * ---------------------------------------------------------------- */

/**
 * @brief Send an encoded audio frame on a track.
 *
 * @p pts_ms is a monotonic timestamp in milliseconds (e.g. millis()).
 * The library converts to RTP clock internally. Both audio and video
 * should use the same clock source for proper A/V synchronization.
 *
 * @param rtc     Initialized RTC state (must be connected).
 * @param mid     Audio track MID.
 * @param pts_ms  Presentation timestamp in milliseconds (monotonic clock).
 * @param data    Encoded audio payload (e.g. Opus frame).
 * @param len     Payload length in bytes.
 * @return NANORTC_OK on success.
 * @retval NANORTC_ERR_WOULD_BLOCK      Output queue or transient TX slots are
 *                                      full. Drain outputs and retry the same
 *                                      frame; RTP sequence/statistics do not
 *                                      advance on this path.
 * @retval NANORTC_ERR_BUFFER_TOO_SMALL RTP/TWCC/SRTP packet cannot fit one
 *                                      configured transmit slot.
 */
NANORTC_API int nanortc_send_audio(nanortc_t *rtc, uint8_t mid, uint32_t pts_ms, const void *data,
                                   size_t len);

#if NANORTC_FEATURE_VIDEO
/**
 * @brief Send a video frame on a track.
 *
 * For H.264: pass an Annex-B access unit. Internally splits NAL units,
 * detects IDR keyframes, sets marker bits, and packetizes via FU-A.
 *
 * @p pts_ms is a monotonic timestamp in milliseconds (e.g. millis()).
 * The library converts to RTP clock (90 kHz) internally.
 *
 * Admission is atomic per frame: the worst-case RTP packet count is
 * computed up front and the frame is rejected before anything is enqueued
 * if it cannot ship whole. A partially sent frame is never put on the
 * wire (truncation guarantees receiver loss → PLI → keyframe storms).
 *
 * @param rtc     Initialized RTC state (must be connected).
 * @param mid     Video track MID.
 * @param pts_ms  Presentation timestamp in milliseconds (monotonic clock).
 * @param data    Video frame (Annex-B for H.264).
 * @param len     Frame length in bytes.
 * @return NANORTC_OK on success.
 * @retval NANORTC_ERR_WOULD_BLOCK      Output queue too full for this frame
 *                                      right now — drain via
 *                                      nanortc_poll_output() and retry.
 *                                      See nanortc_output_free_slots().
 * @retval NANORTC_ERR_BUFFER_TOO_SMALL Frame can never fit: it fragments
 *                                      into more packets than
 *                                      min(NANORTC_OUT_QUEUE_SIZE,
 *                                      NANORTC_VIDEO_PKT_RING_SIZE). Lower
 *                                      the encoder bitrate / bound IDR size
 *                                      or enlarge the rings.
 */
NANORTC_API int nanortc_send_video(nanortc_t *rtc, uint8_t mid, uint32_t pts_ms, const void *data,
                                   size_t len);
#endif /* NANORTC_FEATURE_VIDEO */

/**
 * @brief Request a keyframe from the remote video sender (RTCP PLI).
 *
 * @param rtc  Initialized RTC state (must be connected).
 * @param mid  Video track MID.
 * @return NANORTC_OK on success.
 * @retval NANORTC_ERR_INVALID_PARAM  Not a video track or invalid MID.
 * @retval NANORTC_ERR_STATE          Not connected.
 * @retval NANORTC_ERR_WOULD_BLOCK    Output queue or transient TX slots are
 *                                    full; drain outputs and retry.
 */
NANORTC_API int nanortc_request_keyframe(nanortc_t *rtc, uint8_t mid);

/* ----------------------------------------------------------------
 * Media statistics and bandwidth estimation
 * ---------------------------------------------------------------- */

/** @brief Per-track RTCP statistics snapshot.
 *
 * Only grows by appending fields to the tail so embedded applications
 * built against an older header continue to read the leading fields
 * correctly. The Phase-9 additions (send_bitrate_bps, send_fps_q8,
 * fraction_lost, estimated_bitrate_bps) sit after the legacy ones. */
typedef struct {
    uint8_t mid;               /**< Media track ID. */
    uint32_t packets_sent;     /**< Total RTP packets sent. */
    uint32_t octets_sent;      /**< Total payload bytes sent. */
    uint32_t packets_received; /**< Total RTP packets received. */
    uint32_t packets_lost;     /**< Estimated packets lost. */
    uint32_t jitter;           /**< Interarrival jitter (RFC 3550 §6.4.1). */
    uint32_t rtt_ms;           /**< Round-trip time estimate from DLSR (ms). */
#if NANORTC_FEATURE_VIDEO
    uint32_t bitrate_bps; /**< BWE estimated bitrate (bps, video only).
                               Alias of estimated_bitrate_bps below. */
#endif
    /* Phase 9 additions (always present; zero when the feature that
     * populates them is disabled). */
    uint32_t estimated_bitrate_bps; /**< Current BWE estimate (bps). */
    uint32_t send_bitrate_bps;      /**< Outgoing RTP bytes/s, 1-second sliding window. */
    uint16_t send_fps_q8;           /**< Outgoing video frames/s as unsigned Q8.8
                                         fixed-point (send_fps_q8 / 256.0). */
    uint8_t fraction_lost;          /**< Last RTCP RR fraction lost (0-255 = 0-100 %). */
    uint8_t reserved_pad;           /**< Reserved; pad to keep explicit layout. */
} nanortc_track_stats_t;

/**
 * @brief Get per-track RTCP statistics.
 *
 * @param rtc    Initialized RTC state.
 * @param mid    Media track ID.
 * @param stats  Output structure (caller-provided).
 * @return NANORTC_OK on success.
 * @retval NANORTC_ERR_INVALID_PARAM  Invalid MID or NULL pointer.
 */
NANORTC_API int nanortc_get_track_stats(const nanortc_t *rtc, uint8_t mid,
                                        nanortc_track_stats_t *stats);

#if NANORTC_FEATURE_VIDEO
/**
 * @brief Get current BWE estimated bitrate.
 *
 * Returns the receiver-estimated maximum bitrate from REMB / TWCC feedback.
 * Applications should use this to adapt encoder bitrate/quality.
 *
 * @param rtc  Initialized RTC state.
 * @return Estimated bitrate in bps, or 0 if unavailable.
 */
NANORTC_API uint32_t nanortc_get_estimated_bitrate(const nanortc_t *rtc);

/**
 * @brief Override BWE min/max bitrate bounds at runtime.
 *
 * The new bounds take effect immediately on the next feedback sample. If
 * the current estimate falls outside the new range it is clamped.
 * Passing 0 for a bound reverts that bound to its compile-time default
 * (NANORTC_BWE_MIN_BITRATE or NANORTC_BWE_MAX_BITRATE).
 *
 * Typical use: embedded camera learns its hardware encoder can only hit
 * 1.5 Mbps and calls nanortc_set_bitrate_bounds(rtc, 100000, 1500000) at
 * init so BWE never suggests a rate the encoder cannot deliver.
 *
 * @param rtc      Initialized RTC state.
 * @param min_bps  New minimum bitrate in bps, or 0 to keep the default.
 * @param max_bps  New maximum bitrate in bps, or 0 to keep the default.
 * @return NANORTC_OK on success, NANORTC_ERR_INVALID_PARAM if @p rtc is
 *         NULL or if @p min_bps > @p max_bps (both non-zero).
 */
NANORTC_API int nanortc_set_bitrate_bounds(nanortc_t *rtc, uint32_t min_bps, uint32_t max_bps);

/**
 * @brief Set the initial BWE estimate before any feedback arrives.
 *
 * Only effective when called before the first REMB or TWCC feedback is
 * received. After that the estimate is driven by feedback. Pass 0 to
 * reset to the compile-time default (NANORTC_BWE_INITIAL_BITRATE).
 *
 * @param rtc  Initialized RTC state.
 * @param bps  Initial estimate in bps, or 0 to reset to default.
 * @return NANORTC_OK on success, NANORTC_ERR_INVALID_PARAM if @p rtc is
 *         NULL.
 */
NANORTC_API int nanortc_set_initial_bitrate(nanortc_t *rtc, uint32_t bps);

/**
 * @brief Set the percentage change that triggers NANORTC_EV_BITRATE_ESTIMATE.
 *
 * Applications polling stats less often may want a larger threshold to
 * avoid event churn; applications doing tight closed-loop control want a
 * smaller one. Pass 0 to reset to the compile-time default
 * (NANORTC_BWE_EVENT_THRESHOLD_PCT).
 *
 * @param rtc  Initialized RTC state.
 * @param pct  Percent threshold (0..100), or 0 to reset to default.
 * @return NANORTC_OK on success, NANORTC_ERR_INVALID_PARAM if @p rtc is
 *         NULL or @p pct > 100.
 */
NANORTC_API int nanortc_set_bwe_event_threshold(nanortc_t *rtc, uint8_t pct);

#if NANORTC_FEATURE_VIDEO_RATE_CONTROL
/**
 * @brief Install the adaptive capability ladder for video spec control.
 *
 * Once a ladder is set, the SDK's pure-compute rate controller runs on every
 * bandwidth-estimate update and emits NANORTC_EV_SPEC_RECOMMENDATION whenever
 * the recommended rung changes (and on the first selection). The application
 * applies the recommended {resolution, fps, bitrate} to its encoder — the SDK
 * decides, the caller acts (core belief #10).
 *
 * The array MUST be ordered ascending by @c bitrate_bps (rung 0 = lowest spec)
 * and is treated as caller-owned and read-only: the library stores only the
 * pointer + count (no copy), so the array MUST outlive the session. Pass
 * @p rungs == NULL or @p count == 0 to clear the ladder (controller idle).
 *
 * @param rtc    Initialized RTC state.
 * @param rungs  Caller-owned ladder array, or NULL to clear.
 * @param count  Number of rungs (0 to clear).
 * @return NANORTC_OK on success, NANORTC_ERR_INVALID_PARAM if @p rtc is NULL
 *         or the ladder is not ascending by bitrate.
 */
NANORTC_API int nanortc_set_capability_ladder(nanortc_t *rtc, const nanortc_spec_rung_t *rungs,
                                              uint8_t count);
#endif /* NANORTC_FEATURE_VIDEO_RATE_CONTROL */
#endif /* NANORTC_FEATURE_VIDEO */

#endif /* NANORTC_HAVE_MEDIA_TRANSPORT */

/* ----------------------------------------------------------------
 * DataChannel API
 * ---------------------------------------------------------------- */

#if NANORTC_FEATURE_DATACHANNEL

/**
 * @brief Create a DataChannel (register in SDP session).
 *
 * Call before nanortc_create_offer(). Pass NULL for @p options to get
 * reliable, ordered delivery (the common default).
 *
 * @param rtc      Initialized RTC state.
 * @param label    Channel label (NUL-terminated, required).
 * @param options  Optional parameters (NULL = reliable, ordered).
 * @return Stream ID (>= 0) on success, negative error on failure.
 */
NANORTC_API int nanortc_create_datachannel(nanortc_t *rtc, const char *label,
                                           const nanortc_datachannel_options_t *options);

/**
 * @brief Send binary data on a DataChannel.
 *
 * @param rtc   Initialized RTC state (must be connected).
 * @param id    SCTP stream ID.
 * @param data  Payload to send.
 * @param len   Payload length in bytes.
 * @return NANORTC_OK on success.
 * @retval NANORTC_ERR_STATE  Not connected.
 * @retval NANORTC_ERR_WOULD_BLOCK  SCTP send buffer full.
 */
NANORTC_API int nanortc_datachannel_send(nanortc_t *rtc, uint16_t id, const void *data, size_t len);

/**
 * @brief Send a UTF-8 string on a DataChannel.
 *
 * @param rtc  Initialized RTC state (must be connected).
 * @param id   SCTP stream ID.
 * @param str  NUL-terminated UTF-8 string.
 * @return NANORTC_OK on success.
 */
NANORTC_API int nanortc_datachannel_send_string(nanortc_t *rtc, uint16_t id, const char *str);

/**
 * @brief Close a DataChannel.
 *
 * @param rtc  Initialized RTC state.
 * @param id   SCTP stream ID.
 * @return NANORTC_OK on success.
 * @retval NANORTC_ERR_INVALID_PARAM  Unknown or closed channel.
 */
NANORTC_API int nanortc_datachannel_close(nanortc_t *rtc, uint16_t id);

/**
 * @brief Get the label of a DataChannel.
 *
 * @param rtc  Initialized RTC state.
 * @param id   SCTP stream ID.
 * @return Label string, or NULL if invalid. Valid until nanortc_destroy().
 */
NANORTC_API const char *nanortc_datachannel_get_label(nanortc_t *rtc, uint16_t id);

#endif /* NANORTC_FEATURE_DATACHANNEL */

/* ----------------------------------------------------------------
 * Connection state API
 * ---------------------------------------------------------------- */

/**
 * @brief Check if the RTC instance is still operational.
 *
 * @param rtc  RTC state, or NULL.
 * @return true if alive (not closed), false if closed or NULL.
 */
NANORTC_API bool nanortc_is_alive(const nanortc_t *rtc);

/**
 * @brief Check if the connection is fully established.
 *
 * @param rtc  RTC state.
 * @return true if state >= CONNECTED.
 */
NANORTC_API bool nanortc_is_connected(const nanortc_t *rtc);

/**
 * @brief Initiate graceful disconnection.
 *
 * Enqueues SCTP SHUTDOWN (if DataChannel) and DTLS close_notify.
 * Continue calling nanortc_poll_output() to drain close frames,
 * then call nanortc_destroy().
 *
 * @param rtc  Initialized RTC state.
 */
NANORTC_API void nanortc_disconnect(nanortc_t *rtc);

/* ----------------------------------------------------------------
 * Diagnostics
 * ---------------------------------------------------------------- */

/**
 * @brief Return a human-readable name for an error code.
 *
 * @param err  Error code (NANORTC_OK, NANORTC_ERR_*, or unknown).
 * @return Static string such as "NANORTC_OK" or "NANORTC_ERR_PARSE". Never NULL.
 */
NANORTC_API const char *nanortc_err_name(int err);

#ifdef __cplusplus
}
#endif

#endif /* NANORTC_H_ */
