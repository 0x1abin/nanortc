/*
 * nanortc — Compile-time configuration defaults
 *
 * Override any value by:
 *   1. Defining NANORTC_CONFIG_FILE as a header path
 *      (e.g. -DNANORTC_CONFIG_FILE=\"my_nanortc_config.h\")
 *   2. Defining individual macros before including this header
 *   3. On ESP-IDF: using Kconfig via `idf.py menuconfig`
 *
 * This follows the same pattern as MBEDTLS_CONFIG_FILE / FreeRTOSConfig.h.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_CONFIG_H_
#define NANORTC_CONFIG_H_

/* ----------------------------------------------------------------
 * User override: include custom config file if specified
 * ---------------------------------------------------------------- */

#ifdef NANORTC_CONFIG_FILE
#include NANORTC_CONFIG_FILE
#endif

/* ----------------------------------------------------------------
 * ESP-IDF Kconfig mapping
 *
 * Include sdkconfig.h to access CONFIG_NANORTC_* Kconfig values.
 * Map CONFIG_NANORTC_* to NANORTC_* macros.
 * ---------------------------------------------------------------- */

#if defined(IDF_VER) || defined(ESP_PLATFORM)
#include "sdkconfig.h"
#endif

#if defined(CONFIG_NANORTC_MAX_DATACHANNELS) && !defined(NANORTC_MAX_DATACHANNELS)
#define NANORTC_MAX_DATACHANNELS CONFIG_NANORTC_MAX_DATACHANNELS
#endif

#if defined(CONFIG_NANORTC_MAX_ICE_CANDIDATES) && !defined(NANORTC_MAX_ICE_CANDIDATES)
#define NANORTC_MAX_ICE_CANDIDATES CONFIG_NANORTC_MAX_ICE_CANDIDATES
#endif

#if defined(CONFIG_NANORTC_ICE_MAX_CHECKS) && !defined(NANORTC_ICE_MAX_CHECKS)
#define NANORTC_ICE_MAX_CHECKS CONFIG_NANORTC_ICE_MAX_CHECKS
#endif

#if defined(CONFIG_NANORTC_ICE_CHECK_INTERVAL_MS) && !defined(NANORTC_ICE_CHECK_INTERVAL_MS)
#define NANORTC_ICE_CHECK_INTERVAL_MS CONFIG_NANORTC_ICE_CHECK_INTERVAL_MS
#endif

#if defined(CONFIG_NANORTC_DTLS_BUF_SIZE) && !defined(NANORTC_DTLS_BUF_SIZE)
#define NANORTC_DTLS_BUF_SIZE CONFIG_NANORTC_DTLS_BUF_SIZE
#endif

#if defined(CONFIG_NANORTC_SDP_BUF_SIZE) && !defined(NANORTC_SDP_BUF_SIZE)
#define NANORTC_SDP_BUF_SIZE CONFIG_NANORTC_SDP_BUF_SIZE
#endif

#if defined(CONFIG_NANORTC_SCTP_SEND_BUF_SIZE) && !defined(NANORTC_SCTP_SEND_BUF_SIZE)
#define NANORTC_SCTP_SEND_BUF_SIZE CONFIG_NANORTC_SCTP_SEND_BUF_SIZE
#endif

#if defined(CONFIG_NANORTC_SCTP_RECV_BUF_SIZE) && !defined(NANORTC_SCTP_RECV_BUF_SIZE)
#define NANORTC_SCTP_RECV_BUF_SIZE CONFIG_NANORTC_SCTP_RECV_BUF_SIZE
#endif

#if defined(CONFIG_NANORTC_OUT_QUEUE_SIZE) && !defined(NANORTC_OUT_QUEUE_SIZE)
#define NANORTC_OUT_QUEUE_SIZE CONFIG_NANORTC_OUT_QUEUE_SIZE
#endif

#if defined(CONFIG_NANORTC_JITTER_SLOTS) && !defined(NANORTC_JITTER_SLOTS)
#define NANORTC_JITTER_SLOTS CONFIG_NANORTC_JITTER_SLOTS
#endif

#if defined(CONFIG_NANORTC_JITTER_SLOT_DATA_SIZE) && !defined(NANORTC_JITTER_SLOT_DATA_SIZE)
#define NANORTC_JITTER_SLOT_DATA_SIZE CONFIG_NANORTC_JITTER_SLOT_DATA_SIZE
#endif

#if defined(CONFIG_NANORTC_MEDIA_BUF_SIZE) && !defined(NANORTC_MEDIA_BUF_SIZE)
#define NANORTC_MEDIA_BUF_SIZE CONFIG_NANORTC_MEDIA_BUF_SIZE
#endif

#if defined(CONFIG_NANORTC_RTCP_INTERVAL_MS) && !defined(NANORTC_RTCP_INTERVAL_MS)
#define NANORTC_RTCP_INTERVAL_MS CONFIG_NANORTC_RTCP_INTERVAL_MS
#endif

#if defined(CONFIG_NANORTC_ICE_UFRAG_SIZE) && !defined(NANORTC_ICE_UFRAG_SIZE)
#define NANORTC_ICE_UFRAG_SIZE CONFIG_NANORTC_ICE_UFRAG_SIZE
#endif

#if defined(CONFIG_NANORTC_ICE_PWD_SIZE) && !defined(NANORTC_ICE_PWD_SIZE)
#define NANORTC_ICE_PWD_SIZE CONFIG_NANORTC_ICE_PWD_SIZE
#endif

#if defined(CONFIG_NANORTC_ICE_REMOTE_UFRAG_SIZE) && !defined(NANORTC_ICE_REMOTE_UFRAG_SIZE)
#define NANORTC_ICE_REMOTE_UFRAG_SIZE CONFIG_NANORTC_ICE_REMOTE_UFRAG_SIZE
#endif

#if defined(CONFIG_NANORTC_ICE_REMOTE_PWD_SIZE) && !defined(NANORTC_ICE_REMOTE_PWD_SIZE)
#define NANORTC_ICE_REMOTE_PWD_SIZE CONFIG_NANORTC_ICE_REMOTE_PWD_SIZE
#endif

#if defined(CONFIG_NANORTC_SDP_FINGERPRINT_SIZE) && !defined(NANORTC_SDP_FINGERPRINT_SIZE)
#define NANORTC_SDP_FINGERPRINT_SIZE CONFIG_NANORTC_SDP_FINGERPRINT_SIZE
#endif

#if defined(CONFIG_NANORTC_SDP_MIN_BUF_SIZE) && !defined(NANORTC_SDP_MIN_BUF_SIZE)
#define NANORTC_SDP_MIN_BUF_SIZE CONFIG_NANORTC_SDP_MIN_BUF_SIZE
#endif

#if defined(CONFIG_NANORTC_DC_LABEL_SIZE) && !defined(NANORTC_DC_LABEL_SIZE)
#define NANORTC_DC_LABEL_SIZE CONFIG_NANORTC_DC_LABEL_SIZE
#endif

#if defined(CONFIG_NANORTC_DC_OUT_BUF_SIZE) && !defined(NANORTC_DC_OUT_BUF_SIZE)
#define NANORTC_DC_OUT_BUF_SIZE CONFIG_NANORTC_DC_OUT_BUF_SIZE
#endif

#if defined(CONFIG_NANORTC_STUN_BUF_SIZE) && !defined(NANORTC_STUN_BUF_SIZE)
#define NANORTC_STUN_BUF_SIZE CONFIG_NANORTC_STUN_BUF_SIZE
#endif

/* Feature flag Kconfig mapping (ESP-IDF booleans) */
#if defined(IDF_VER) && !defined(NANORTC_FEATURE_DATACHANNEL)
#ifdef CONFIG_NANORTC_FEATURE_DATACHANNEL
#define NANORTC_FEATURE_DATACHANNEL 1
#else
#define NANORTC_FEATURE_DATACHANNEL 0
#endif
#endif

#if defined(IDF_VER) && !defined(NANORTC_FEATURE_DC_RELIABLE)
#ifdef CONFIG_NANORTC_FEATURE_DC_RELIABLE
#define NANORTC_FEATURE_DC_RELIABLE 1
#else
#define NANORTC_FEATURE_DC_RELIABLE 0
#endif
#endif

#if defined(IDF_VER) && !defined(NANORTC_FEATURE_DC_ORDERED)
#ifdef CONFIG_NANORTC_FEATURE_DC_ORDERED
#define NANORTC_FEATURE_DC_ORDERED 1
#else
#define NANORTC_FEATURE_DC_ORDERED 0
#endif
#endif

#if defined(IDF_VER) && !defined(NANORTC_FEATURE_AUDIO)
#ifdef CONFIG_NANORTC_FEATURE_AUDIO
#define NANORTC_FEATURE_AUDIO 1
#else
#define NANORTC_FEATURE_AUDIO 0
#endif
#endif

#if defined(IDF_VER) && !defined(NANORTC_FEATURE_VIDEO)
#ifdef CONFIG_NANORTC_FEATURE_VIDEO
#define NANORTC_FEATURE_VIDEO 1
#else
#define NANORTC_FEATURE_VIDEO 0
#endif
#endif

/* ----------------------------------------------------------------
 * Feature flags (orthogonal, user-configurable)
 *
 * Each flag enables a set of modules independently:
 *   NANORTC_FEATURE_DATACHANNEL  — SCTP + DCEP
 *   NANORTC_FEATURE_DC_RELIABLE  — retransmission (sub-feature of DC)
 *   NANORTC_FEATURE_DC_ORDERED   — SSN-based ordered delivery (sub-feature of DC)
 *   NANORTC_FEATURE_AUDIO        — Audio (RTP/SRTP/Jitter)
 *   NANORTC_FEATURE_VIDEO        — Video (RTP/SRTP/BWE)
 * ---------------------------------------------------------------- */

/** @brief Enable DataChannel (SCTP + DCEP). Default: 1. */
#ifndef NANORTC_FEATURE_DATACHANNEL
#define NANORTC_FEATURE_DATACHANNEL 1
#endif

/** @brief Enable SCTP retransmission. Sub-feature of DataChannel. */
#ifndef NANORTC_FEATURE_DC_RELIABLE
#define NANORTC_FEATURE_DC_RELIABLE NANORTC_FEATURE_DATACHANNEL
#endif

/** @brief Enable SSN-based ordered delivery. Sub-feature of DataChannel. */
#ifndef NANORTC_FEATURE_DC_ORDERED
#define NANORTC_FEATURE_DC_ORDERED NANORTC_FEATURE_DATACHANNEL
#endif

/** @brief Enable audio transport (RTP/SRTP + jitter buffer). Default: 0. */
#ifndef NANORTC_FEATURE_AUDIO
#define NANORTC_FEATURE_AUDIO 0
#endif

/** @brief Enable video transport (RTP/SRTP + BWE). Default: 0. */
#ifndef NANORTC_FEATURE_VIDEO
#define NANORTC_FEATURE_VIDEO 0
#endif

/* Derived (internal): true when any media transport is needed */
#if NANORTC_FEATURE_AUDIO || NANORTC_FEATURE_VIDEO
#define NANORTC_HAVE_MEDIA_TRANSPORT 1
#else
#define NANORTC_HAVE_MEDIA_TRANSPORT 0
#endif

/* ----------------------------------------------------------------
 * DataChannel limits
 * ---------------------------------------------------------------- */

#ifndef NANORTC_MAX_DATACHANNELS
#define NANORTC_MAX_DATACHANNELS 8
#endif

#ifndef NANORTC_DC_LABEL_SIZE
#define NANORTC_DC_LABEL_SIZE 32
#endif

#ifndef NANORTC_DC_OUT_BUF_SIZE
#define NANORTC_DC_OUT_BUF_SIZE 128
#endif

/* ----------------------------------------------------------------
 * ICE limits
 * ---------------------------------------------------------------- */

#ifndef NANORTC_MAX_ICE_CANDIDATES
#define NANORTC_MAX_ICE_CANDIDATES 4
#endif

#ifndef NANORTC_ICE_MAX_CHECKS
#define NANORTC_ICE_MAX_CHECKS 25
#endif

/* ICE connectivity check pacing interval in milliseconds (RFC 8445) */
#ifndef NANORTC_ICE_CHECK_INTERVAL_MS
#define NANORTC_ICE_CHECK_INTERVAL_MS 50
#endif

/* ICE credential lengths (chars, excluding NUL) */
#define NANORTC_ICE_UFRAG_LEN 8  /* 4 random bytes → 8 hex chars */
#define NANORTC_ICE_PWD_LEN   22 /* 11 random bytes → 22 hex chars */

/* ICE credential buffer sizes (must fit LEN + NUL) */

#ifndef NANORTC_ICE_UFRAG_SIZE
#define NANORTC_ICE_UFRAG_SIZE (NANORTC_ICE_UFRAG_LEN + 1)
#endif

#ifndef NANORTC_ICE_PWD_SIZE
#define NANORTC_ICE_PWD_SIZE (NANORTC_ICE_PWD_LEN + 2) /* +1 NUL +1 pad for alignment */
#endif

#ifndef NANORTC_ICE_REMOTE_UFRAG_SIZE
#define NANORTC_ICE_REMOTE_UFRAG_SIZE 32
#endif

#ifndef NANORTC_ICE_REMOTE_PWD_SIZE
#define NANORTC_ICE_REMOTE_PWD_SIZE 128
#endif

/* ----------------------------------------------------------------
 * DTLS buffer size
 * ---------------------------------------------------------------- */

#ifndef NANORTC_DTLS_BUF_SIZE
#define NANORTC_DTLS_BUF_SIZE 2048
#endif

/* ----------------------------------------------------------------
 * SDP buffer size
 * ---------------------------------------------------------------- */

#ifndef NANORTC_SDP_BUF_SIZE
#define NANORTC_SDP_BUF_SIZE 2048
#endif

/* Maximum ICE candidates parsed from a single SDP offer/answer */
#ifndef NANORTC_SDP_MAX_CANDIDATES
#define NANORTC_SDP_MAX_CANDIDATES 8
#endif

/* SDP field sizes */

#ifndef NANORTC_SDP_FINGERPRINT_SIZE
#define NANORTC_SDP_FINGERPRINT_SIZE 128
#endif

#ifndef NANORTC_SDP_MIN_BUF_SIZE
#define NANORTC_SDP_MIN_BUF_SIZE 256
#endif

/* ----------------------------------------------------------------
 * STUN scratch buffer size
 * ---------------------------------------------------------------- */

#ifndef NANORTC_STUN_BUF_SIZE
#define NANORTC_STUN_BUF_SIZE 256
#endif

/* ----------------------------------------------------------------
 * SCTP configuration (RFC 4960)
 * ---------------------------------------------------------------- */

#ifndef NANORTC_SCTP_SEND_BUF_SIZE
#define NANORTC_SCTP_SEND_BUF_SIZE 4096
#endif

#ifndef NANORTC_SCTP_RECV_BUF_SIZE
#define NANORTC_SCTP_RECV_BUF_SIZE 4096
#endif

/* Maximum SCTP packet size over DTLS */
#ifndef NANORTC_SCTP_MTU
#define NANORTC_SCTP_MTU 1200
#endif

/* Pending outbound DATA chunk queue depth */
#ifndef NANORTC_SCTP_MAX_SEND_QUEUE
#define NANORTC_SCTP_MAX_SEND_QUEUE 16
#endif

/* Maximum gap ack blocks in SACK */
#ifndef NANORTC_SCTP_MAX_GAP_BLOCKS
#define NANORTC_SCTP_MAX_GAP_BLOCKS 4
#endif

/* State cookie maximum size (bytes) */
#ifndef NANORTC_SCTP_COOKIE_SIZE
#define NANORTC_SCTP_COOKIE_SIZE 32
#endif

/* Retransmission timer defaults (RFC 4960 §6.3.1) */
#ifndef NANORTC_SCTP_RTO_INITIAL_MS
#define NANORTC_SCTP_RTO_INITIAL_MS 1000
#endif

#ifndef NANORTC_SCTP_RTO_MIN_MS
#define NANORTC_SCTP_RTO_MIN_MS 200
#endif

#ifndef NANORTC_SCTP_RTO_MAX_MS
#define NANORTC_SCTP_RTO_MAX_MS 10000
#endif

/* Maximum DATA chunk retransmissions before error */
#ifndef NANORTC_SCTP_MAX_RETRANSMITS
#define NANORTC_SCTP_MAX_RETRANSMITS 10
#endif

/* Maximum INIT retransmissions (RFC 4960 §5.1) */
#ifndef NANORTC_SCTP_MAX_INIT_RETRANSMITS
#define NANORTC_SCTP_MAX_INIT_RETRANSMITS 8
#endif

/* HEARTBEAT interval in milliseconds (RFC 4960 §8.3) */
#ifndef NANORTC_SCTP_HEARTBEAT_INTERVAL_MS
#define NANORTC_SCTP_HEARTBEAT_INTERVAL_MS 30000
#endif

/* SCTP output queue depth (number of buffered outgoing packets, power of 2) */
#ifndef NANORTC_SCTP_OUT_QUEUE_SIZE
#define NANORTC_SCTP_OUT_QUEUE_SIZE 4
#endif

/* ----------------------------------------------------------------
 * Output queue depth (must be power of 2 for ring buffer masking)
 * ---------------------------------------------------------------- */

#ifndef NANORTC_OUT_QUEUE_SIZE
#define NANORTC_OUT_QUEUE_SIZE 8
#endif

/* ----------------------------------------------------------------
 * Media transport configuration
 * ---------------------------------------------------------------- */

/* Media scratch buffer size (for RTP/SRTP processing) */
#ifndef NANORTC_MEDIA_BUF_SIZE
#define NANORTC_MEDIA_BUF_SIZE 1500
#endif

/* RTCP send interval in milliseconds (RFC 3550 §6.2) */
#ifndef NANORTC_RTCP_INTERVAL_MS
#define NANORTC_RTCP_INTERVAL_MS 5000
#endif

/* ----------------------------------------------------------------
 * Jitter buffer slots (AUDIO feature only)
 * ---------------------------------------------------------------- */

#ifndef NANORTC_JITTER_SLOTS
#define NANORTC_JITTER_SLOTS 64
#endif

/* Maximum RTP packet data per jitter slot (bytes) */
#ifndef NANORTC_JITTER_SLOT_DATA_SIZE
#define NANORTC_JITTER_SLOT_DATA_SIZE 1500
#endif

/* ----------------------------------------------------------------
 * Logging configuration
 * ---------------------------------------------------------------- */

/**
 * @brief Maximum compile-time log level.
 *
 * Messages above this level are eliminated by the preprocessor.
 * Set via compiler flag: -DNANORTC_LOG_LEVEL=0 (errors only).
 * Values: 0=ERROR, 1=WARN, 2=INFO, 3=DEBUG, 4=TRACE.
 */
#ifndef NANORTC_LOG_LEVEL
#define NANORTC_LOG_LEVEL 4 /* TRACE — all messages compiled in */
#endif

/**
 * @brief Define to compile out all logging code entirely.
 */
/* #define NANORTC_LOG_DISABLED */

/**
 * @brief Define to omit source file/line/function from log messages.
 *
 * Reduces code size on very constrained targets.
 */
/* #define NANORTC_LOG_NO_LOC */

/* ----------------------------------------------------------------
 * Protocol-fixed address sizes
 *
 * Defined here (not nanortc.h) so internal headers can use them
 * without creating a circular include with nanortc.h.
 * ---------------------------------------------------------------- */

#define NANORTC_ADDR_SIZE     16 /**< IPv6 binary address length (RFC 4291). */
#define NANORTC_IPV6_STR_SIZE 46 /**< Max IPv6 string length (INET6_ADDRSTRLEN). */

/* ----------------------------------------------------------------
 * SDP media direction
 *
 * Defined here so nano_sdp.h can embed it without including nanortc.h.
 * ---------------------------------------------------------------- */

/** @brief SDP media direction attribute. */
typedef enum {
    NANORTC_DIR_SENDRECV, /**< Send and receive. */
    NANORTC_DIR_SENDONLY, /**< Send only. */
    NANORTC_DIR_RECVONLY, /**< Receive only. */
    NANORTC_DIR_INACTIVE, /**< Neither send nor receive. */
} nanortc_direction_t;

/* ----------------------------------------------------------------
 * Compile-time validation
 * ---------------------------------------------------------------- */

#if (NANORTC_OUT_QUEUE_SIZE & (NANORTC_OUT_QUEUE_SIZE - 1)) != 0
#error "NANORTC_OUT_QUEUE_SIZE must be a power of 2"
#endif

#if NANORTC_FEATURE_DATACHANNEL && NANORTC_MAX_DATACHANNELS < 1
#error "NANORTC_MAX_DATACHANNELS must be at least 1"
#endif

#if NANORTC_DTLS_BUF_SIZE < 256
#error "NANORTC_DTLS_BUF_SIZE must be at least 256"
#endif

#if NANORTC_ICE_UFRAG_SIZE < (NANORTC_ICE_UFRAG_LEN + 1)
#error "NANORTC_ICE_UFRAG_SIZE must be at least NANORTC_ICE_UFRAG_LEN + 1"
#endif

#if NANORTC_ICE_PWD_SIZE < (NANORTC_ICE_PWD_LEN + 1)
#error "NANORTC_ICE_PWD_SIZE must be at least NANORTC_ICE_PWD_LEN + 1"
#endif

#if NANORTC_STUN_BUF_SIZE < 128
#error "NANORTC_STUN_BUF_SIZE must be at least 128"
#endif

#if NANORTC_SDP_MIN_BUF_SIZE < 128
#error "NANORTC_SDP_MIN_BUF_SIZE must be at least 128"
#endif

#endif /* NANORTC_CONFIG_H_ */
