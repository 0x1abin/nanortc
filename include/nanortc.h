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
/** @brief Data for NANORTC_EV_BITRATE_ESTIMATE: BWE estimate changed. */
typedef struct {
    uint32_t bitrate_bps;      /**< Current estimated bitrate (bps). */
    uint32_t prev_bitrate_bps; /**< Previous estimated bitrate (bps). */
} nanortc_ev_bitrate_estimate_t;
#endif

/** @brief Data for NANORTC_EV_ICE_CANDIDATE (trickle ICE). */
typedef struct {
    const char *candidate_str; /**< SDP candidate line (valid until next poll). */
    bool end_of_candidates;    /**< True = no more local candidates. */
} nanortc_ev_ice_candidate_t;

/** @brief Data for NANORTC_EV_DATACHANNEL_OPEN. */
typedef struct {
    uint16_t id;       /**< SCTP stream ID. */
    const char *label; /**< Channel label (valid until next poll). */
} nanortc_ev_datachannel_open_t;

/** @brief Data for NANORTC_EV_DATACHANNEL_DATA. */
typedef struct {
    uint16_t id;         /**< SCTP stream ID. */
    const uint8_t *data; /**< Payload pointer (valid until next poll). */
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
 * Pointer fields are valid only until the next call to
 * nanortc_poll_output() or nanortc_handle_input(). Copy if needed.
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

/** @brief Output item produced by nanortc_poll_output(). Tagged union on @c type. */
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
#endif

#if NANORTC_FEATURE_VIDEO
    /** Packet ring: each output queue slot gets its own buffer so video FU-A
     *  fragments don't clobber each other before dispatch_outputs sends them. */
    uint8_t pkt_ring[NANORTC_OUT_QUEUE_SIZE][NANORTC_MEDIA_BUF_SIZE];

    /** NACK retransmission metadata — tracks which RTP seq lives in each
     *  pkt_ring slot so we can retransmit on RTCP NACK (RFC 4585 §6.2.1).
     *  len==0 means the slot is empty/invalid. */
    struct {
        uint16_t seq; /**< RTP sequence number stored in this slot. */
        uint16_t len; /**< SRTP-protected packet length (0 = invalid). */
    } pkt_ring_meta[NANORTC_OUT_QUEUE_SIZE];

    /** Shared bandwidth estimator (session-wide, not per-track). */
    nano_bwe_t bwe;
#endif

    /* Output queue (simple ring buffer) */
    nanortc_output_t out_queue[NANORTC_OUT_QUEUE_SIZE];
    uint16_t out_head;
    uint16_t out_tail;

    /* Scratch buffer for STUN encode/decode.
     * Sans I/O contract: caller must drain outputs before next handle_receive. */
    uint8_t stun_buf[NANORTC_STUN_BUF_SIZE];

    /* Scratch buffer for DTLS output polling */
    uint8_t dtls_scratch[NANORTC_DTLS_BUF_SIZE];

    /* Stored remote address for SCTP output routing */
    nanortc_addr_t remote_addr;

    /* Scratch for trickle ICE candidate strings (valid until next poll) */
#if NANORTC_FEATURE_TURN
    char relay_cand_str[NANORTC_IPV6_STR_SIZE + 96];
#endif
    char srflx_cand_str[NANORTC_IPV6_STR_SIZE + 96];
    char host_cand_str[NANORTC_IPV6_STR_SIZE + 96];

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
 * Always processes pending timers (ICE checks, SCTP retransmits). If @p data
 * is non-NULL, also demuxes and processes the incoming packet (RFC 7983).
 *
 * Call with data=NULL, len=0, src=NULL for a pure timeout (no packet).
 *
 * @note For correct timing-sensitive behaviour (DTLS handshake retransmit,
 *       ICE connectivity checks, SCTP retransmit, consent freshness), this
 *       function should be called at least every
 *       @c NANORTC_MIN_POLL_INTERVAL_MS (default 50 ms) even when no packet
 *       is pending — pass @c data=NULL and @c len=0 to tick timers only.
 *       Slower polling rates may miss DTLS retransmit deadlines and delay
 *       handshake completion. A future @c nanortc_next_timeout_ms() API
 *       will let callers use epoll_wait/select with an exact timeout.
 *
 * @param rtc     Initialized RTC state.
 * @param now_ms  Current monotonic time in milliseconds.
 * @param data    Packet payload, or NULL for timeout-only.
 * @param len     Payload length in bytes (0 if data is NULL).
 * @param src     Source address of the packet, or NULL for timeout-only.
 * @return NANORTC_OK on success.
 */
NANORTC_API int nanortc_handle_input(nanortc_t *rtc, uint32_t now_ms, const uint8_t *data,
                                     size_t len, const nanortc_addr_t *src);

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

/**
 * @brief Look up the current MID of the nth active track of a given kind.
 *
 * The MID a pre-added track carries may change across nanortc_accept_offer():
 * in answerer mode, the offer drives the m-line order, so pre-added tracks
 * are relabeled to match the offer's corresponding m-line.  Callers that
 * cached a MID from nanortc_add_audio_track() / nanortc_add_video_track()
 * should refresh it with this function after nanortc_accept_offer() returns.
 *
 * @param rtc   Initialized RTC state.
 * @param kind  NANORTC_TRACK_AUDIO or NANORTC_TRACK_VIDEO.
 * @param nth   0-based index among active tracks of that kind.
 * @return MID (>= 0) on success, negative if no such track.
 */
NANORTC_API int nanortc_find_track_mid(const nanortc_t *rtc, nanortc_track_kind_t kind,
                                       uint8_t nth);

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
 * @param rtc     Initialized RTC state (must be connected).
 * @param mid     Video track MID.
 * @param pts_ms  Presentation timestamp in milliseconds (monotonic clock).
 * @param data    Video frame (Annex-B for H.264).
 * @param len     Frame length in bytes.
 * @return NANORTC_OK on success.
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
 */
NANORTC_API int nanortc_request_keyframe(nanortc_t *rtc, uint8_t mid);

/* ----------------------------------------------------------------
 * Media statistics and bandwidth estimation
 * ---------------------------------------------------------------- */

/** @brief Per-track RTCP statistics snapshot. */
typedef struct {
    uint8_t mid;               /**< Media track ID. */
    uint32_t packets_sent;     /**< Total RTP packets sent. */
    uint32_t octets_sent;      /**< Total payload bytes sent. */
    uint32_t packets_received; /**< Total RTP packets received. */
    uint32_t packets_lost;     /**< Estimated packets lost. */
    uint32_t jitter;           /**< Interarrival jitter (RFC 3550 §6.4.1). */
    uint32_t rtt_ms;           /**< Round-trip time estimate from DLSR (ms). */
#if NANORTC_FEATURE_VIDEO
    uint32_t bitrate_bps; /**< BWE estimated bitrate (bps, video only). */
#endif
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
 * Returns the receiver-estimated maximum bitrate from REMB feedback.
 * Applications should use this to adapt encoder bitrate/quality.
 *
 * @param rtc  Initialized RTC state.
 * @return Estimated bitrate in bps, or 0 if unavailable.
 */
NANORTC_API uint32_t nanortc_get_estimated_bitrate(const nanortc_t *rtc);
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
