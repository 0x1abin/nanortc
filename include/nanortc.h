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
#define NANO_API __attribute__((visibility("default")))
#else
#define NANO_API
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
static inline uint16_t nano_htons(uint16_t x)
{
    return (uint16_t)((x >> 8) | (x << 8));
}

/**
 * @brief Convert 16-bit value from network to host byte order.
 * @param x Network-order (big-endian) value.
 * @return Host-order value.
 */
static inline uint16_t nano_ntohs(uint16_t x)
{
    return nano_htons(x);
}

/**
 * @brief Convert 32-bit value from host to network byte order.
 * @param x Host-order value.
 * @return Network-order (big-endian) value.
 */
static inline uint32_t nano_htonl(uint32_t x)
{
    return ((x >> 24) & 0x000000FFu) | ((x >> 8) & 0x0000FF00u) | ((x << 8) & 0x00FF0000u) |
           ((x << 24) & 0xFF000000u);
}

/**
 * @brief Convert 32-bit value from network to host byte order.
 * @param x Network-order (big-endian) value.
 * @return Host-order value.
 */
static inline uint32_t nano_ntohl(uint32_t x)
{
    return nano_htonl(x);
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
static inline uint16_t nano_read_u16be(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

/**
 * @brief Read a 32-bit big-endian value from an unaligned buffer.
 * @param p Pointer to at least 4 bytes.
 * @return Host-order 32-bit value.
 */
static inline uint32_t nano_read_u32be(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/**
 * @brief Write a 16-bit value in big-endian format to an unaligned buffer.
 * @param p Pointer to at least 2 bytes of output.
 * @param v Host-order value to write.
 */
static inline void nano_write_u16be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v);
}

/**
 * @brief Write a 32-bit value in big-endian format to an unaligned buffer.
 * @param p Pointer to at least 4 bytes of output.
 * @param v Host-order value to write.
 */
static inline void nano_write_u32be(uint8_t *p, uint32_t v)
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
#define NANO_OK                   0  /**< Success. */
#define NANO_ERR_INVALID_PARAM    -1 /**< NULL pointer or out-of-range argument. */
#define NANO_ERR_BUFFER_TOO_SMALL -2 /**< Caller-provided buffer is too small. */
#define NANO_ERR_STATE            -3 /**< Invalid state for this operation. */
#define NANO_ERR_CRYPTO           -4 /**< Cryptographic operation failed. */
#define NANO_ERR_PROTOCOL         -5 /**< Protocol violation (remote peer). */
#define NANO_ERR_NOT_IMPLEMENTED  -6 /**< Feature not compiled in or not yet implemented. */
#define NANO_ERR_PARSE            -7 /**< Malformed input data. */
#define NANO_ERR_NO_DATA          -8 /**< No data available (non-fatal). */
#define NANO_ERR_INTERNAL         -9 /**< Internal logic error (bug). */
/** @} */

/* Configuration limits are defined in nanortc_config.h */

/* ----------------------------------------------------------------
 * Logging types
 * ---------------------------------------------------------------- */

/** @brief Log severity levels. */
typedef enum {
    NANO_LOG_ERROR = 0, /**< Unrecoverable errors that prevent operation. */
    NANO_LOG_WARN = 1,  /**< Unusual but recoverable conditions. */
    NANO_LOG_INFO = 2,  /**< Normal operation milestones. */
    NANO_LOG_DEBUG = 3, /**< Diagnostic information. */
    NANO_LOG_TRACE = 4, /**< Detailed packet-level diagnostics. */
} nano_log_level_t;

/**
 * @brief Structured log message passed to the user callback.
 *
 * All pointer fields are valid only during the callback invocation.
 * The application must copy any data it needs to retain.
 */
typedef struct nano_log_message {
    nano_log_level_t level; /**< Severity level. */
    const char *subsystem;  /**< Component tag (e.g. "ICE", "SCTP"). */
    const char *message;    /**< Human-readable message (static string). */
    const char *file;       /**< Source file name, or NULL. */
    uint32_t line;          /**< Source line number, or 0. */
    const char *function;   /**< Function name, or NULL. */
} nano_log_message_t;

/**
 * @brief Log callback function type.
 *
 * Called synchronously for each log message at or below the configured
 * level. Must not call NanoRTC functions (no re-entrancy).
 *
 * @param msg   Pointer to the log message (never NULL).
 * @param ctx   User-supplied context pointer from nano_log_config_t.
 */
typedef void (*nano_log_fn_t)(const nano_log_message_t *msg, void *ctx);

/**
 * @brief Logging subsystem configuration.
 *
 * Embed in nano_rtc_config_t. Set callback to NULL to disable logging.
 * The compile-time NANO_LOG_LEVEL caps the runtime level.
 */
typedef struct nano_log_config {
    nano_log_level_t level; /**< Runtime minimum level (capped by NANO_LOG_LEVEL). */
    nano_log_fn_t callback; /**< Log callback, or NULL to disable. */
    void *user_data;        /**< Opaque pointer passed to callback. */
} nano_log_config_t;

/* ----------------------------------------------------------------
 * Forward declarations
 * ---------------------------------------------------------------- */

#ifndef NANO_CRYPTO_PROVIDER_T_DECLARED
#define NANO_CRYPTO_PROVIDER_T_DECLARED
typedef struct nano_crypto_provider nano_crypto_provider_t;
#endif

/* ----------------------------------------------------------------
 * Address type (network-agnostic)
 * ---------------------------------------------------------------- */

/** @brief Network-agnostic socket address (IPv4 / IPv6). */
#ifndef NANO_ADDR_T_DECLARED
#define NANO_ADDR_T_DECLARED
typedef struct nano_addr nano_addr_t;
#endif

/* Protocol-fixed address sizes */
#define NANO_ADDR_SIZE     16 /**< IPv6 binary address length (RFC 4291). */
#define NANO_IPV6_STR_SIZE 46 /**< Max IPv6 string length (INET6_ADDRSTRLEN). */

struct nano_addr {
    uint8_t family;               /**< Address family: 4 = IPv4, 6 = IPv6. */
    uint8_t addr[NANO_ADDR_SIZE]; /**< Binary address (network byte order). */
    uint16_t port;                /**< Port number (host byte order). */
};

/* ----------------------------------------------------------------
 * Output / Event enums
 * ---------------------------------------------------------------- */

/** @brief Type of output produced by nano_poll_output(). */
typedef enum {
    NANO_OUTPUT_TRANSMIT, /**< UDP data to send to the network. */
    NANO_OUTPUT_EVENT,    /**< Application-level event. */
    NANO_OUTPUT_TIMEOUT,  /**< Requested callback delay in milliseconds. */
} nano_output_type_t;

/** @brief Application event types delivered via NANO_OUTPUT_EVENT. */
typedef enum {
    /* Connection lifecycle */
    NANO_EVENT_ICE_CONNECTED = 0,  /**< ICE connectivity check succeeded. */
    NANO_EVENT_DTLS_CONNECTED = 1, /**< DTLS handshake completed. */
    NANO_EVENT_SCTP_CONNECTED = 2, /**< SCTP association established. */
    NANO_EVENT_DISCONNECTED = 3,   /**< Peer disconnected or timeout. */

    /* DataChannel */
    NANO_EVENT_DATACHANNEL_OPEN = 4,   /**< DataChannel opened (DCEP complete). */
    NANO_EVENT_DATACHANNEL_CLOSE = 5,  /**< DataChannel closed. */
    NANO_EVENT_DATACHANNEL_DATA = 6,   /**< Binary data received on DataChannel. */
    NANO_EVENT_DATACHANNEL_STRING = 7, /**< UTF-8 string received on DataChannel. */

    /* Audio */
    NANO_EVENT_AUDIO_DATA = 8, /**< Decoded audio frame available. */

    /* Video */
    NANO_EVENT_VIDEO_DATA = 9,        /**< Decoded video frame available. */
    NANO_EVENT_KEYFRAME_REQUEST = 10, /**< Remote peer requests a keyframe. */
} nano_event_type_t;

/* ----------------------------------------------------------------
 * Event structure
 * ---------------------------------------------------------------- */

/** @brief Application event delivered through nano_poll_output(). */
typedef struct nano_event {
    nano_event_type_t type; /**< Event type discriminator. */
    uint16_t stream_id;     /**< DataChannel stream ID (DC events only). */
    const uint8_t *data;    /**< Payload pointer (valid until next poll). */
    size_t len;             /**< Payload length in bytes. */
    uint32_t timestamp;     /**< RTP timestamp (media events only). */
    bool is_keyframe;       /**< True if video frame is a keyframe. */
} nano_event_t;

/* ----------------------------------------------------------------
 * Output structure
 * ---------------------------------------------------------------- */

/** @brief Output item produced by nano_poll_output(). Tagged union on @c type. */
typedef struct nano_output {
    nano_output_type_t type; /**< Discriminator for the anonymous union. */
    union {
        struct {
            const uint8_t *data; /**< Packet payload. */
            size_t len;          /**< Packet length in bytes. */
            nano_addr_t dest;    /**< Destination address. */
        } transmit;              /**< Valid when type == NANO_OUTPUT_TRANSMIT. */
        nano_event_t event;      /**< Valid when type == NANO_OUTPUT_EVENT. */
        uint32_t timeout_ms;     /**< Valid when type == NANO_OUTPUT_TIMEOUT. */
    };
} nano_output_t;

/* ----------------------------------------------------------------
 * Direction (media)
 * ---------------------------------------------------------------- */

/** @brief SDP media direction attribute. */
typedef enum {
    NANO_DIR_SENDRECV, /**< Send and receive. */
    NANO_DIR_SENDONLY, /**< Send only. */
    NANO_DIR_RECVONLY, /**< Receive only. */
    NANO_DIR_INACTIVE, /**< Neither send nor receive. */
} nano_direction_t;

/* ----------------------------------------------------------------
 * Codec identifiers
 * ---------------------------------------------------------------- */

/** @brief Supported audio/video codec identifiers for SDP negotiation. */
typedef enum {
    NANO_CODEC_NONE = 0, /**< No codec selected. */
    NANO_CODEC_OPUS,     /**< Opus audio (RFC 6716). */
    NANO_CODEC_PCMA,     /**< G.711 A-law (RFC 3551). */
    NANO_CODEC_PCMU,     /**< G.711 mu-law (RFC 3551). */
    NANO_CODEC_H264,     /**< H.264 video (RFC 6184). */
    NANO_CODEC_VP8,      /**< VP8 video (RFC 7741). */
} nano_codec_t;

/* ----------------------------------------------------------------
 * ICE role
 * ---------------------------------------------------------------- */

/** @brief ICE agent role (RFC 8445). */
typedef enum {
    NANO_ROLE_CONTROLLED,  /**< Answerer: respond to STUN checks (ICE-Lite). */
    NANO_ROLE_CONTROLLING, /**< Offerer: initiate STUN connectivity checks. */
} nano_ice_role_t;

/* ----------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------- */

/** @brief Configuration passed to nano_rtc_init(). */
typedef struct nano_rtc_config {
    const nano_crypto_provider_t *crypto; /**< Crypto backend (required, non-NULL). */
    nano_ice_role_t role;                 /**< ICE role (controlled or controlling). */

    /** @brief Logging configuration (optional, zero-init disables). */
    nano_log_config_t log;

    /* Memory configuration */
    uint32_t sctp_send_buf_size; /**< SCTP send buffer size (0 = default). */
    uint32_t sctp_recv_buf_size; /**< SCTP receive buffer size (0 = default). */

#if NANO_FEATURE_AUDIO
    nano_codec_t audio_codec;         /**< Audio codec to negotiate. */
    uint32_t audio_sample_rate;       /**< Sample rate in Hz (e.g. 48000). */
    uint8_t audio_channels;           /**< Number of audio channels. */
    nano_direction_t audio_direction; /**< Audio direction attribute. */
    uint32_t jitter_depth_ms;         /**< Jitter buffer depth in ms. */
#endif

#if NANO_FEATURE_VIDEO
    nano_codec_t video_codec;         /**< Video codec to negotiate. */
    nano_direction_t video_direction; /**< Video direction attribute. */
#endif
} nano_rtc_config_t;

/* ----------------------------------------------------------------
 * Main state machine (opaque internals)
 * ---------------------------------------------------------------- */

/** @brief Opaque RTC state machine. Defined in nano_rtc_internal.h. */
typedef struct nano_rtc nano_rtc_t;

/* ----------------------------------------------------------------
 * Lifecycle API
 * ---------------------------------------------------------------- */

/**
 * @brief Initialize the RTC state machine.
 *
 * @param rtc  Caller-allocated state (must be zeroed before first call).
 * @param cfg  Configuration (pointer contents copied; need not persist).
 * @return NANO_OK on success.
 * @retval NANO_ERR_INVALID_PARAM  @p rtc or @p cfg is NULL, or crypto missing.
 */
NANO_API int nano_rtc_init(nano_rtc_t *rtc, const nano_rtc_config_t *cfg);

/**
 * @brief Release resources held by the RTC state machine.
 * @param rtc  State previously initialized with nano_rtc_init(), or NULL (no-op).
 */
NANO_API void nano_rtc_destroy(nano_rtc_t *rtc);

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
 * @return NANO_OK on success.
 * @retval NANO_ERR_BUFFER_TOO_SMALL  @p answer_buf_len is insufficient.
 * @retval NANO_ERR_PARSE             Malformed SDP offer.
 */
NANO_API int nano_accept_offer(nano_rtc_t *rtc, const char *offer, char *answer_buf,
                               size_t answer_buf_len, size_t *out_len);

/**
 * @brief Generate an SDP offer (controlling/offerer role).
 *
 * @param rtc           Initialized RTC state.
 * @param offer_buf     Buffer to receive the SDP offer.
 * @param offer_buf_len Size of @p offer_buf in bytes.
 * @param out_len       Receives offer length in bytes (may be NULL).
 * @return NANO_OK on success.
 * @retval NANO_ERR_BUFFER_TOO_SMALL  @p offer_buf_len is insufficient.
 */
NANO_API int nano_create_offer(nano_rtc_t *rtc, char *offer_buf, size_t offer_buf_len,
                               size_t *out_len);

/**
 * @brief Parse a remote SDP answer (after creating an offer).
 *
 * @param rtc     Initialized RTC state (must have called nano_create_offer first).
 * @param answer  NUL-terminated remote SDP answer string.
 * @return NANO_OK on success.
 * @retval NANO_ERR_PARSE  Malformed SDP answer.
 * @retval NANO_ERR_STATE  No pending offer.
 */
NANO_API int nano_accept_answer(nano_rtc_t *rtc, const char *answer);

/* ----------------------------------------------------------------
 * ICE API
 * ---------------------------------------------------------------- */

/**
 * @brief Add a local ICE candidate (host address).
 *
 * @param rtc   Initialized RTC state.
 * @param ip    NUL-terminated IP address string (IPv4 or IPv6).
 * @param port  UDP port number (host byte order).
 * @return NANO_OK on success.
 * @retval NANO_ERR_INVALID_PARAM  @p ip is NULL or invalid.
 * @retval NANO_ERR_BUFFER_TOO_SMALL  Candidate table full.
 */
NANO_API int nano_add_local_candidate(nano_rtc_t *rtc, const char *ip, uint16_t port);

/**
 * @brief Add a remote ICE candidate from an SDP candidate attribute.
 *
 * @param rtc            Initialized RTC state.
 * @param candidate_str  NUL-terminated SDP candidate line (a=candidate:...).
 * @return NANO_OK on success.
 * @retval NANO_ERR_PARSE  Malformed candidate string.
 */
NANO_API int nano_add_remote_candidate(nano_rtc_t *rtc, const char *candidate_str);

/* ----------------------------------------------------------------
 * Event loop (Sans I/O core)
 * ---------------------------------------------------------------- */

/**
 * @brief Dequeue the next output from the state machine.
 *
 * Call in a loop until it returns NANO_ERR_NO_DATA.
 *
 * @param rtc  Initialized RTC state.
 * @param out  Receives the next output item.
 * @return NANO_OK if @p out was filled.
 * @retval NANO_ERR_NO_DATA  No more pending outputs.
 */
NANO_API int nano_poll_output(nano_rtc_t *rtc, nano_output_t *out);

/**
 * @brief Feed an incoming UDP packet into the state machine.
 *
 * @param rtc     Initialized RTC state.
 * @param now_ms  Current monotonic time in milliseconds.
 * @param data    Packet payload.
 * @param len     Payload length in bytes.
 * @param src     Source address of the packet.
 * @return NANO_OK on success.
 */
NANO_API int nano_handle_receive(nano_rtc_t *rtc, uint32_t now_ms, const uint8_t *data, size_t len,
                                 const nano_addr_t *src);

/**
 * @brief Advance timers in the state machine.
 *
 * Call when the timeout requested by NANO_OUTPUT_TIMEOUT expires.
 *
 * @param rtc     Initialized RTC state.
 * @param now_ms  Current monotonic time in milliseconds.
 * @return NANO_OK on success.
 */
NANO_API int nano_handle_timeout(nano_rtc_t *rtc, uint32_t now_ms);

/* ----------------------------------------------------------------
 * DataChannel API
 * ---------------------------------------------------------------- */

#if NANO_FEATURE_DATACHANNEL
/**
 * @brief Send binary data on a DataChannel.
 *
 * @param rtc        Initialized RTC state.
 * @param stream_id  SCTP stream ID of the DataChannel.
 * @param data       Payload to send.
 * @param len        Payload length in bytes.
 * @return NANO_OK on success.
 * @retval NANO_ERR_STATE  DataChannel not open.
 */
NANO_API int nano_send_datachannel(nano_rtc_t *rtc, uint16_t stream_id, const void *data,
                                   size_t len);

/**
 * @brief Send a UTF-8 string on a DataChannel.
 *
 * @param rtc        Initialized RTC state.
 * @param stream_id  SCTP stream ID of the DataChannel.
 * @param str        NUL-terminated UTF-8 string to send.
 * @return NANO_OK on success.
 * @retval NANO_ERR_STATE  DataChannel not open.
 */
NANO_API int nano_send_datachannel_string(nano_rtc_t *rtc, uint16_t stream_id, const char *str);
#endif

/* ----------------------------------------------------------------
 * Media API
 * ---------------------------------------------------------------- */

#if NANO_FEATURE_AUDIO
/**
 * @brief Send an audio frame.
 *
 * @param rtc        Initialized RTC state.
 * @param timestamp  RTP timestamp for this frame.
 * @param data       Encoded audio payload.
 * @param len        Payload length in bytes.
 * @return NANO_OK on success.
 */
NANO_API int nano_send_audio(nano_rtc_t *rtc, uint32_t timestamp, const void *data, size_t len);
#endif

#if NANO_FEATURE_VIDEO
/**
 * @brief Send a video frame.
 *
 * @param rtc          Initialized RTC state.
 * @param timestamp    RTP timestamp for this frame.
 * @param data         Encoded video payload.
 * @param len          Payload length in bytes.
 * @param is_keyframe  Non-zero if this is a keyframe (IDR).
 * @return NANO_OK on success.
 */
NANO_API int nano_send_video(nano_rtc_t *rtc, uint32_t timestamp, const void *data, size_t len,
                             int is_keyframe);

/**
 * @brief Request a keyframe from the remote video sender (RTCP FIR/PLI).
 *
 * @param rtc  Initialized RTC state.
 * @return NANO_OK on success.
 */
NANO_API int nano_request_keyframe(nano_rtc_t *rtc);
#endif

/* ----------------------------------------------------------------
 * Diagnostics
 * ---------------------------------------------------------------- */

/**
 * @brief Return a human-readable name for an error code.
 *
 * @param err  Error code (NANO_OK, NANO_ERR_*, or unknown).
 * @return Static string such as "NANO_OK" or "NANO_ERR_PARSE". Never NULL.
 */
NANO_API const char *nano_err_to_name(int err);

#ifdef __cplusplus
}
#endif

#endif /* NANORTC_H_ */
