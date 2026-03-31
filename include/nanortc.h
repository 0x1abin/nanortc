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
    NANORTC_EV_CONNECTED = 0,             /**< ICE+DTLS(+SCTP) fully established. */
    NANORTC_EV_DISCONNECTED = 1,          /**< Connection lost or closed. */
    NANORTC_EV_ICE_STATE_CHANGE = 2,      /**< ICE state transition. */
    NANORTC_EV_MEDIA_ADDED = 3,           /**< Remote added new media track. */
    NANORTC_EV_MEDIA_CHANGED = 4,         /**< Media direction changed. */
    NANORTC_EV_MEDIA_DATA = 5,            /**< Received media frame (audio or video). */
    NANORTC_EV_KEYFRAME_REQUEST = 6,      /**< Remote requests keyframe (PLI/FIR). */
    NANORTC_EV_CHANNEL_OPEN = 7,          /**< DataChannel opened (DCEP complete). */
    NANORTC_EV_CHANNEL_DATA = 8,          /**< DataChannel data received. */
    NANORTC_EV_CHANNEL_CLOSE = 9,         /**< DataChannel closed. */
    NANORTC_EV_CHANNEL_BUFFERED_LOW = 10, /**< Send buffer drained below threshold. */
} nanortc_event_type_t;

/* ----------------------------------------------------------------
 * Per-event data structures (str0m-inspired typed events)
 * ---------------------------------------------------------------- */

/** @brief Data for NANORTC_EV_MEDIA_ADDED: remote added a new media track. */
typedef struct {
    uint8_t mid;                   /**< Media ID (track index). */
    uint8_t kind;                  /**< nano_media_kind_t: NANO_MEDIA_AUDIO or NANO_MEDIA_VIDEO. */
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

/** @brief Data for NANORTC_EV_CHANNEL_OPEN. */
typedef struct {
    uint16_t id;       /**< SCTP stream ID. */
    const char *label; /**< Channel label (valid until next poll). */
} nanortc_ev_channel_open_t;

/** @brief Data for NANORTC_EV_CHANNEL_DATA. */
typedef struct {
    uint16_t id;         /**< SCTP stream ID. */
    const uint8_t *data; /**< Payload pointer (valid until next poll). */
    size_t len;          /**< Payload length in bytes. */
    bool binary;         /**< true = binary, false = UTF-8 string. */
} nanortc_ev_channel_data_t;

/** @brief Data for NANORTC_EV_CHANNEL_CLOSE / NANORTC_EV_CHANNEL_BUFFERED_LOW. */
typedef struct {
    uint16_t id; /**< SCTP stream ID. */
} nanortc_ev_channel_id_t;

/* ----------------------------------------------------------------
 * Event structure (tagged union)
 * ---------------------------------------------------------------- */

/**
 * @brief Application event delivered through nanortc_poll_output().
 *
 * Pointer fields are valid only until the next call to
 * nanortc_poll_output() or nanortc_handle_receive(). Copy if needed.
 */
typedef struct nanortc_event {
    nanortc_event_type_t type; /**< Event type discriminator. */
    union {
        nanortc_ev_media_added_t media_added;           /**< EV_MEDIA_ADDED */
        nanortc_ev_media_changed_t media_changed;       /**< EV_MEDIA_CHANGED */
        nanortc_ev_media_data_t media_data;             /**< EV_MEDIA_DATA */
        nanortc_ev_keyframe_request_t keyframe_request; /**< EV_KEYFRAME_REQUEST */
        nanortc_ev_channel_open_t channel_open;         /**< EV_CHANNEL_OPEN */
        nanortc_ev_channel_data_t channel_data;         /**< EV_CHANNEL_DATA */
        nanortc_ev_channel_id_t channel_id; /**< EV_CHANNEL_CLOSE, EV_CHANNEL_BUFFERED_LOW */
        uint16_t ice_state;                 /**< EV_ICE_STATE_CHANGE */
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
 * Configuration
 * ---------------------------------------------------------------- */

/** @brief Configuration passed to nanortc_init(). */
typedef struct nanortc_config {
    const nanortc_crypto_provider_t *crypto; /**< Crypto backend (required, non-NULL). */
    nanortc_ice_role_t role;                 /**< ICE role (controlled or controlling). */

    /** @brief Logging configuration (optional, zero-init disables). */
    nanortc_log_config_t log;

    /* Memory configuration */
    uint32_t sctp_send_buf_size; /**< SCTP send buffer size (0 = default). */
    uint32_t sctp_recv_buf_size; /**< SCTP receive buffer size (0 = default). */

#if NANORTC_FEATURE_AUDIO
    uint32_t jitter_depth_ms; /**< Jitter buffer depth in ms (default for new audio tracks). */
#endif
} nanortc_config_t;

/* ----------------------------------------------------------------
 * Internal subsystem types (needed for struct layout — do not use directly)
 * ---------------------------------------------------------------- */
#include "nano_ice.h"
#include "nano_dtls.h"
#include "nano_sdp.h"

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

#if NANORTC_FEATURE_DATACHANNEL
    nano_sctp_t sctp;
    nano_dc_t datachannel;
#endif

#if NANORTC_HAVE_MEDIA_TRANSPORT
    /** Media tracks (str0m-inspired: indexed by MID). */
    nano_media_t media[NANORTC_MAX_MEDIA_TRACKS];
    uint8_t media_count; /**< Number of allocated media track slots. */

    /** SSRC → MID lookup table for RTP receive-path demuxing. */
    nano_ssrc_entry_t ssrc_map[NANORTC_MAX_SSRC_MAP];

    /** Shared SRTP session (keys shared across all tracks in BUNDLE). */
    nano_srtp_t srtp;
#endif

#if NANORTC_FEATURE_VIDEO
    /** Packet ring: each output queue slot gets its own buffer so video FU-A
     *  fragments don't clobber each other before dispatch_outputs sends them. */
    uint8_t pkt_ring[NANORTC_OUT_QUEUE_SIZE][NANORTC_MEDIA_BUF_SIZE];

    /** Shared bandwidth estimator (session-wide, not per-track). */
    nano_bwe_t bwe;
#endif

    /* Output queue (simple ring buffer) */
    nanortc_output_t out_queue[NANORTC_OUT_QUEUE_SIZE];
    uint8_t out_head;
    uint8_t out_tail;

    /* Scratch buffer for STUN encode/decode.
     * Sans I/O contract: caller must drain outputs before next handle_receive. */
    uint8_t stun_buf[NANORTC_STUN_BUF_SIZE];

    /* Scratch buffer for DTLS output polling */
    uint8_t dtls_scratch[NANORTC_DTLS_BUF_SIZE];

    /* Stored remote address for SCTP output routing */
    nanortc_addr_t remote_addr;
};

typedef struct nanortc nanortc_t;

/* ----------------------------------------------------------------
 * Handle types (str0m-inspired Writer / Channel pattern)
 * ---------------------------------------------------------------- */

/** @brief Media writer handle. Obtain via nanortc_writer(). */
typedef struct {
    nanortc_t *rtc; /**< Parent RTC state (do not modify). */
    uint8_t mid;    /**< Track MID. */
} nano_writer_t;

/** @brief DataChannel handle. Obtain via nanortc_channel(). */
typedef struct {
    nanortc_t *rtc; /**< Parent RTC state (do not modify). */
    uint16_t id;    /**< SCTP stream ID. */
} nano_channel_t;

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
 * @param rtc            Initialized RTC state.
 * @param candidate_str  NUL-terminated SDP candidate line (a=candidate:...).
 * @return NANORTC_OK on success.
 * @retval NANORTC_ERR_PARSE  Malformed candidate string.
 */
NANORTC_API int nanortc_add_remote_candidate(nanortc_t *rtc, const char *candidate_str);

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
 * @brief Feed an incoming UDP packet into the state machine.
 *
 * @param rtc     Initialized RTC state.
 * @param now_ms  Current monotonic time in milliseconds.
 * @param data    Packet payload.
 * @param len     Payload length in bytes.
 * @param src     Source address of the packet.
 * @return NANORTC_OK on success.
 */
NANORTC_API int nanortc_handle_receive(nanortc_t *rtc, uint32_t now_ms, const uint8_t *data,
                                       size_t len, const nanortc_addr_t *src);

/**
 * @brief Advance timers in the state machine.
 *
 * Call when the timeout requested by NANORTC_OUTPUT_TIMEOUT expires.
 *
 * @param rtc     Initialized RTC state.
 * @param now_ms  Current monotonic time in milliseconds.
 * @return NANORTC_OK on success.
 */
NANORTC_API int nanortc_handle_timeout(nanortc_t *rtc, uint32_t now_ms);

/* ----------------------------------------------------------------
 * DataChannel types
 * ---------------------------------------------------------------- */

#if NANORTC_FEATURE_DATACHANNEL

/** @brief DataChannel configuration for nanortc_add_channel_ex(). */
typedef struct nanortc_datachannel_config {
    const char *label;        /**< Channel label (NUL-terminated, required). */
    bool ordered;             /**< Ordered delivery (default: true). */
    uint16_t max_retransmits; /**< Max retransmit count (0 = reliable). */
} nanortc_datachannel_config_t;

#endif

/* ----------------------------------------------------------------
 * Media API (multi-track, str0m-inspired)
 * ---------------------------------------------------------------- */

#if NANORTC_HAVE_MEDIA_TRANSPORT

/** Flags for nanortc_writer_write() when sending video. */
#define NANORTC_VIDEO_FLAG_KEYFRAME 0x01 /**< This NAL is part of a keyframe (IDR). */
#define NANORTC_VIDEO_FLAG_MARKER   0x02 /**< Last NAL in access unit (sets RTP marker bit). */

/**
 * @brief Add a media track (audio or video) to the SDP session.
 *
 * Call before nanortc_create_offer() or nanortc_accept_offer(). Each call
 * adds one SDP m-line. Returns the MID (media ID) which is the track handle.
 *
 * @param rtc         Initialized RTC state.
 * @param kind        NANO_MEDIA_AUDIO or NANO_MEDIA_VIDEO.
 * @param direction   Send/receive direction for this track.
 * @param codec       Codec to negotiate (NANORTC_CODEC_OPUS, etc.).
 * @param sample_rate Audio sample rate in Hz (e.g. 48000), 0 for video.
 * @param channels    Audio channels (1 or 2), 0 for video.
 * @return MID (>= 0) on success, negative error code on failure.
 */
NANORTC_API int nanortc_add_media(nanortc_t *rtc, nano_media_kind_t kind,
                                  nanortc_direction_t direction, nanortc_codec_t codec,
                                  uint32_t sample_rate, uint8_t channels);

/**
 * @brief Change direction of an existing media track.
 *
 * @param rtc  Initialized RTC state.
 * @param mid  Track MID returned by nanortc_add_media().
 * @param dir  New direction.
 */
NANORTC_API void nanortc_set_direction(nanortc_t *rtc, uint8_t mid, nanortc_direction_t dir);

/**
 * @brief Get read-only access to a media track's state.
 *
 * @param rtc  Initialized RTC state.
 * @param mid  Track MID.
 * @return Pointer to internal media state, or NULL if MID is invalid.
 *         Valid until next SDP negotiation or destroy.
 */
NANORTC_API const nano_media_t *nanortc_media(const nanortc_t *rtc, uint8_t mid);

/**
 * @brief Obtain a writer handle for sending media on a track.
 *
 * Validates that the track exists and direction allows sending.
 *
 * @param rtc  Initialized RTC state.
 * @param mid  Track MID.
 * @param w    Receives the writer handle.
 * @return NANORTC_OK on success.
 * @retval NANORTC_ERR_INVALID_PARAM  Track not found.
 * @retval NANORTC_ERR_STATE          Direction is recvonly/inactive, or not connected.
 */
NANORTC_API int nanortc_writer(nanortc_t *rtc, uint8_t mid, nano_writer_t *w);

/**
 * @brief Send media data through a writer handle.
 *
 * For audio: @p data is an encoded frame, @p flags is 0.
 * For video: @p data is a raw H.264 NAL unit, @p flags is NANORTC_VIDEO_FLAG_*.
 *
 * @param w          Writer handle from nanortc_writer().
 * @param timestamp  RTP timestamp.
 * @param data       Encoded payload.
 * @param len        Payload length in bytes.
 * @param flags      Video flags (0 for audio).
 * @return NANORTC_OK on success.
 */
NANORTC_API int nanortc_writer_write(nano_writer_t *w, uint32_t timestamp, const void *data,
                                     size_t len, int flags);

/**
 * @brief Request a keyframe from the remote video sender (RTCP PLI).
 *
 * @param w  Writer handle for a video track.
 * @return NANORTC_OK on success.
 */
NANORTC_API int nanortc_writer_request_keyframe(nano_writer_t *w);

#endif /* NANORTC_HAVE_MEDIA_TRANSPORT */

/* ----------------------------------------------------------------
 * DataChannel API (str0m Channel handle pattern)
 * ---------------------------------------------------------------- */

#if NANORTC_FEATURE_DATACHANNEL

/**
 * @brief Register a DataChannel in the SDP session (before offer/answer).
 *
 * @param rtc    Initialized RTC state.
 * @param label  Channel label (NUL-terminated).
 * @return Stream ID (>= 0) on success, negative error on failure.
 */
NANORTC_API int nanortc_add_channel(nanortc_t *rtc, const char *label);

/**
 * @brief Register a DataChannel with custom configuration.
 *
 * @param rtc  Initialized RTC state.
 * @param cfg  Channel configuration.
 * @return Stream ID (>= 0) on success, negative error on failure.
 */
NANORTC_API int nanortc_add_channel_ex(nanortc_t *rtc, const nanortc_datachannel_config_t *cfg);

/**
 * @brief Obtain a channel handle for DataChannel I/O.
 *
 * @param rtc  Initialized RTC state.
 * @param id   SCTP stream ID.
 * @param ch   Receives the channel handle.
 * @return NANORTC_OK on success.
 * @retval NANORTC_ERR_INVALID_PARAM  Unknown or closed channel.
 */
NANORTC_API int nanortc_channel(nanortc_t *rtc, uint16_t id, nano_channel_t *ch);

/**
 * @brief Send binary data on a DataChannel.
 *
 * @param ch    Channel handle from nanortc_channel().
 * @param data  Payload to send.
 * @param len   Payload length in bytes.
 * @return NANORTC_OK on success.
 */
NANORTC_API int nanortc_channel_send(nano_channel_t *ch, const void *data, size_t len);

/**
 * @brief Send a UTF-8 string on a DataChannel.
 *
 * @param ch   Channel handle from nanortc_channel().
 * @param str  NUL-terminated UTF-8 string.
 * @return NANORTC_OK on success.
 */
NANORTC_API int nanortc_channel_send_string(nano_channel_t *ch, const char *str);

/**
 * @brief Close a DataChannel.
 *
 * @param ch  Channel handle.
 * @return NANORTC_OK on success.
 */
NANORTC_API int nanortc_channel_close(nano_channel_t *ch);

/**
 * @brief Get the label of a DataChannel.
 *
 * @param ch  Channel handle.
 * @return Label string, or NULL if invalid. Valid until nanortc_destroy().
 */
NANORTC_API const char *nanortc_channel_label(nano_channel_t *ch);

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
