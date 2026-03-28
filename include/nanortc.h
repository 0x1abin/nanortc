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
#define NANORTC_OK                   0  /**< Success. */
#define NANORTC_ERR_INVALID_PARAM    -1 /**< NULL pointer or out-of-range argument. */
#define NANORTC_ERR_BUFFER_TOO_SMALL -2 /**< Caller-provided buffer is too small. */
#define NANORTC_ERR_STATE            -3 /**< Invalid state for this operation. */
#define NANORTC_ERR_CRYPTO           -4 /**< Cryptographic operation failed. */
#define NANORTC_ERR_PROTOCOL         -5 /**< Protocol violation (remote peer). */
#define NANORTC_ERR_NOT_IMPLEMENTED  -6 /**< Feature not compiled in or not yet implemented. */
#define NANORTC_ERR_PARSE            -7 /**< Malformed input data. */
#define NANORTC_ERR_NO_DATA          -8 /**< No data available (non-fatal). */
#define NANORTC_ERR_INTERNAL         -9 /**< Internal logic error (bug). */
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

/* Protocol-fixed address sizes */
#define NANORTC_ADDR_SIZE     16 /**< IPv6 binary address length (RFC 4291). */
#define NANORTC_IPV6_STR_SIZE 46 /**< Max IPv6 string length (INET6_ADDRSTRLEN). */

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
    /* Connection lifecycle */
    NANORTC_EVENT_ICE_CONNECTED = 0,  /**< ICE connectivity check succeeded. */
    NANORTC_EVENT_DTLS_CONNECTED = 1, /**< DTLS handshake completed. */
    NANORTC_EVENT_SCTP_CONNECTED = 2, /**< SCTP association established. */
    NANORTC_EVENT_DISCONNECTED = 3,   /**< Peer disconnected or timeout. */

    /* DataChannel */
    NANORTC_EVENT_DATACHANNEL_OPEN = 4,   /**< DataChannel opened (DCEP complete). */
    NANORTC_EVENT_DATACHANNEL_CLOSE = 5,  /**< DataChannel closed. */
    NANORTC_EVENT_DATACHANNEL_DATA = 6,   /**< Binary data received on DataChannel. */
    NANORTC_EVENT_DATACHANNEL_STRING = 7, /**< UTF-8 string received on DataChannel. */

    /* Audio */
    NANORTC_EVENT_AUDIO_DATA = 8, /**< Decoded audio frame available. */

    /* Video */
    NANORTC_EVENT_VIDEO_DATA = 9,        /**< Decoded video frame available. */
    NANORTC_EVENT_KEYFRAME_REQUEST = 10, /**< Remote peer requests a keyframe. */
} nanortc_event_type_t;

/* ----------------------------------------------------------------
 * Event structure
 * ---------------------------------------------------------------- */

/** @brief Application event delivered through nanortc_poll_output(). */
typedef struct nanortc_event {
    nanortc_event_type_t type; /**< Event type discriminator. */
    uint16_t stream_id;        /**< DataChannel stream ID (DC events only). */
    const uint8_t *data;       /**< Payload pointer (valid until next poll). */
    size_t len;                /**< Payload length in bytes. */
    uint32_t timestamp;        /**< RTP timestamp (media events only). */
    bool is_keyframe;          /**< True if video frame is a keyframe. */
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

/* ----------------------------------------------------------------
 * Direction (media)
 * ---------------------------------------------------------------- */

/** @brief SDP media direction attribute. */
typedef enum {
    NANORTC_DIR_SENDRECV, /**< Send and receive. */
    NANORTC_DIR_SENDONLY, /**< Send only. */
    NANORTC_DIR_RECVONLY, /**< Receive only. */
    NANORTC_DIR_INACTIVE, /**< Neither send nor receive. */
} nanortc_direction_t;

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
    nanortc_codec_t audio_codec;         /**< Audio codec to negotiate. */
    uint32_t audio_sample_rate;          /**< Sample rate in Hz (e.g. 48000). */
    uint8_t audio_channels;              /**< Number of audio channels. */
    nanortc_direction_t audio_direction; /**< Audio direction attribute. */
    uint32_t jitter_depth_ms;            /**< Jitter buffer depth in ms. */
#endif

#if NANORTC_FEATURE_VIDEO
    nanortc_codec_t video_codec;         /**< Video codec to negotiate. */
    nanortc_direction_t video_direction; /**< Video direction attribute. */
#endif
} nanortc_config_t;

/* ----------------------------------------------------------------
 * Main state machine (opaque internals)
 * ---------------------------------------------------------------- */

/** @brief Opaque RTC state machine. Defined in nano_rtc_internal.h. */
typedef struct nanortc nanortc_t;

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
 * DataChannel API
 * ---------------------------------------------------------------- */

#if NANORTC_FEATURE_DATACHANNEL
/**
 * @brief Send binary data on a DataChannel.
 *
 * @param rtc        Initialized RTC state.
 * @param stream_id  SCTP stream ID of the DataChannel.
 * @param data       Payload to send.
 * @param len        Payload length in bytes.
 * @return NANORTC_OK on success.
 * @retval NANORTC_ERR_STATE  DataChannel not open.
 */
NANORTC_API int nanortc_send_datachannel(nanortc_t *rtc, uint16_t stream_id, const void *data,
                                         size_t len);

/**
 * @brief Send a UTF-8 string on a DataChannel.
 *
 * @param rtc        Initialized RTC state.
 * @param stream_id  SCTP stream ID of the DataChannel.
 * @param str        NUL-terminated UTF-8 string to send.
 * @return NANORTC_OK on success.
 * @retval NANORTC_ERR_STATE  DataChannel not open.
 */
NANORTC_API int nanortc_send_datachannel_string(nanortc_t *rtc, uint16_t stream_id,
                                                const char *str);
#endif

/* ----------------------------------------------------------------
 * Media API
 * ---------------------------------------------------------------- */

#if NANORTC_FEATURE_AUDIO
/**
 * @brief Send an audio frame.
 *
 * @param rtc        Initialized RTC state.
 * @param timestamp  RTP timestamp for this frame.
 * @param data       Encoded audio payload.
 * @param len        Payload length in bytes.
 * @return NANORTC_OK on success.
 */
NANORTC_API int nanortc_send_audio(nanortc_t *rtc, uint32_t timestamp, const void *data,
                                   size_t len);
#endif

#if NANORTC_FEATURE_VIDEO
/**
 * @brief Send a video frame.
 *
 * @param rtc          Initialized RTC state.
 * @param timestamp    RTP timestamp for this frame.
 * @param data         Encoded video payload.
 * @param len          Payload length in bytes.
 * @param is_keyframe  Non-zero if this is a keyframe (IDR).
 * @return NANORTC_OK on success.
 */
NANORTC_API int nanortc_send_video(nanortc_t *rtc, uint32_t timestamp, const void *data, size_t len,
                                   int is_keyframe);

/**
 * @brief Request a keyframe from the remote video sender (RTCP FIR/PLI).
 *
 * @param rtc  Initialized RTC state.
 * @return NANORTC_OK on success.
 */
NANORTC_API int nanortc_request_keyframe(nanortc_t *rtc);
#endif

/* ----------------------------------------------------------------
 * Diagnostics
 * ---------------------------------------------------------------- */

/**
 * @brief Return a human-readable name for an error code.
 *
 * @param err  Error code (NANORTC_OK, NANORTC_ERR_*, or unknown).
 * @return Static string such as "NANORTC_OK" or "NANORTC_ERR_PARSE". Never NULL.
 */
NANORTC_API const char *nanortc_err_to_name(int err);

#ifdef __cplusplus
}
#endif

#endif /* NANORTC_H_ */
