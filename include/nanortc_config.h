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
 * When built as an ESP-IDF component, sdkconfig.h is auto-included
 * by the toolchain. Map CONFIG_NANO_* to NANO_* macros.
 * ---------------------------------------------------------------- */

#if defined(CONFIG_NANO_MAX_DATACHANNELS) && !defined(NANO_MAX_DATACHANNELS)
#define NANO_MAX_DATACHANNELS CONFIG_NANO_MAX_DATACHANNELS
#endif

#if defined(CONFIG_NANO_MAX_ICE_CANDIDATES) && !defined(NANO_MAX_ICE_CANDIDATES)
#define NANO_MAX_ICE_CANDIDATES CONFIG_NANO_MAX_ICE_CANDIDATES
#endif

#if defined(CONFIG_NANO_ICE_MAX_CHECKS) && !defined(NANO_ICE_MAX_CHECKS)
#define NANO_ICE_MAX_CHECKS CONFIG_NANO_ICE_MAX_CHECKS
#endif

#if defined(CONFIG_NANO_ICE_CHECK_INTERVAL_MS) && !defined(NANO_ICE_CHECK_INTERVAL_MS)
#define NANO_ICE_CHECK_INTERVAL_MS CONFIG_NANO_ICE_CHECK_INTERVAL_MS
#endif

#if defined(CONFIG_NANO_DTLS_BUF_SIZE) && !defined(NANO_DTLS_BUF_SIZE)
#define NANO_DTLS_BUF_SIZE CONFIG_NANO_DTLS_BUF_SIZE
#endif

#if defined(CONFIG_NANO_SDP_BUF_SIZE) && !defined(NANO_SDP_BUF_SIZE)
#define NANO_SDP_BUF_SIZE CONFIG_NANO_SDP_BUF_SIZE
#endif

#if defined(CONFIG_NANO_SCTP_SEND_BUF_SIZE) && !defined(NANO_SCTP_SEND_BUF_SIZE)
#define NANO_SCTP_SEND_BUF_SIZE CONFIG_NANO_SCTP_SEND_BUF_SIZE
#endif

#if defined(CONFIG_NANO_SCTP_RECV_BUF_SIZE) && !defined(NANO_SCTP_RECV_BUF_SIZE)
#define NANO_SCTP_RECV_BUF_SIZE CONFIG_NANO_SCTP_RECV_BUF_SIZE
#endif

#if defined(CONFIG_NANO_OUT_QUEUE_SIZE) && !defined(NANO_OUT_QUEUE_SIZE)
#define NANO_OUT_QUEUE_SIZE CONFIG_NANO_OUT_QUEUE_SIZE
#endif

#if defined(CONFIG_NANO_JITTER_SLOTS) && !defined(NANO_JITTER_SLOTS)
#define NANO_JITTER_SLOTS CONFIG_NANO_JITTER_SLOTS
#endif

#if defined(CONFIG_NANO_ICE_UFRAG_SIZE) && !defined(NANO_ICE_UFRAG_SIZE)
#define NANO_ICE_UFRAG_SIZE CONFIG_NANO_ICE_UFRAG_SIZE
#endif

#if defined(CONFIG_NANO_ICE_PWD_SIZE) && !defined(NANO_ICE_PWD_SIZE)
#define NANO_ICE_PWD_SIZE CONFIG_NANO_ICE_PWD_SIZE
#endif

#if defined(CONFIG_NANO_ICE_REMOTE_UFRAG_SIZE) && !defined(NANO_ICE_REMOTE_UFRAG_SIZE)
#define NANO_ICE_REMOTE_UFRAG_SIZE CONFIG_NANO_ICE_REMOTE_UFRAG_SIZE
#endif

#if defined(CONFIG_NANO_ICE_REMOTE_PWD_SIZE) && !defined(NANO_ICE_REMOTE_PWD_SIZE)
#define NANO_ICE_REMOTE_PWD_SIZE CONFIG_NANO_ICE_REMOTE_PWD_SIZE
#endif

#if defined(CONFIG_NANO_SDP_FINGERPRINT_SIZE) && !defined(NANO_SDP_FINGERPRINT_SIZE)
#define NANO_SDP_FINGERPRINT_SIZE CONFIG_NANO_SDP_FINGERPRINT_SIZE
#endif

#if defined(CONFIG_NANO_SDP_MIN_BUF_SIZE) && !defined(NANO_SDP_MIN_BUF_SIZE)
#define NANO_SDP_MIN_BUF_SIZE CONFIG_NANO_SDP_MIN_BUF_SIZE
#endif

#if defined(CONFIG_NANO_DC_LABEL_SIZE) && !defined(NANO_DC_LABEL_SIZE)
#define NANO_DC_LABEL_SIZE CONFIG_NANO_DC_LABEL_SIZE
#endif

#if defined(CONFIG_NANO_DC_OUT_BUF_SIZE) && !defined(NANO_DC_OUT_BUF_SIZE)
#define NANO_DC_OUT_BUF_SIZE CONFIG_NANO_DC_OUT_BUF_SIZE
#endif

#if defined(CONFIG_NANO_STUN_BUF_SIZE) && !defined(NANO_STUN_BUF_SIZE)
#define NANO_STUN_BUF_SIZE CONFIG_NANO_STUN_BUF_SIZE
#endif

/* Feature flag Kconfig mapping (ESP-IDF booleans) */
#if defined(IDF_VER) && !defined(NANO_FEATURE_DATACHANNEL)
#ifdef CONFIG_NANO_FEATURE_DATACHANNEL
#define NANO_FEATURE_DATACHANNEL 1
#else
#define NANO_FEATURE_DATACHANNEL 0
#endif
#endif

#if defined(IDF_VER) && !defined(NANO_FEATURE_DC_RELIABLE)
#ifdef CONFIG_NANO_FEATURE_DC_RELIABLE
#define NANO_FEATURE_DC_RELIABLE 1
#else
#define NANO_FEATURE_DC_RELIABLE 0
#endif
#endif

#if defined(IDF_VER) && !defined(NANO_FEATURE_DC_ORDERED)
#ifdef CONFIG_NANO_FEATURE_DC_ORDERED
#define NANO_FEATURE_DC_ORDERED 1
#else
#define NANO_FEATURE_DC_ORDERED 0
#endif
#endif

#if defined(IDF_VER) && !defined(NANO_FEATURE_AUDIO)
#ifdef CONFIG_NANO_FEATURE_AUDIO
#define NANO_FEATURE_AUDIO 1
#else
#define NANO_FEATURE_AUDIO 0
#endif
#endif

#if defined(IDF_VER) && !defined(NANO_FEATURE_VIDEO)
#ifdef CONFIG_NANO_FEATURE_VIDEO
#define NANO_FEATURE_VIDEO 1
#else
#define NANO_FEATURE_VIDEO 0
#endif
#endif

/* ----------------------------------------------------------------
 * Feature flags (orthogonal, user-configurable)
 *
 * Each flag enables a set of modules independently:
 *   NANO_FEATURE_DATACHANNEL  — SCTP + DCEP
 *   NANO_FEATURE_DC_RELIABLE  — retransmission (sub-feature of DC)
 *   NANO_FEATURE_DC_ORDERED   — SSN-based ordered delivery (sub-feature of DC)
 *   NANO_FEATURE_AUDIO        — Audio (RTP/SRTP/Jitter)
 *   NANO_FEATURE_VIDEO        — Video (RTP/SRTP/BWE)
 * ---------------------------------------------------------------- */

/** @brief Enable DataChannel (SCTP + DCEP). Default: 1. */
#ifndef NANO_FEATURE_DATACHANNEL
#define NANO_FEATURE_DATACHANNEL 1
#endif

/** @brief Enable SCTP retransmission. Sub-feature of DataChannel. */
#ifndef NANO_FEATURE_DC_RELIABLE
#define NANO_FEATURE_DC_RELIABLE NANO_FEATURE_DATACHANNEL
#endif

/** @brief Enable SSN-based ordered delivery. Sub-feature of DataChannel. */
#ifndef NANO_FEATURE_DC_ORDERED
#define NANO_FEATURE_DC_ORDERED NANO_FEATURE_DATACHANNEL
#endif

/** @brief Enable audio transport (RTP/SRTP + jitter buffer). Default: 0. */
#ifndef NANO_FEATURE_AUDIO
#define NANO_FEATURE_AUDIO 0
#endif

/** @brief Enable video transport (RTP/SRTP + BWE). Default: 0. */
#ifndef NANO_FEATURE_VIDEO
#define NANO_FEATURE_VIDEO 0
#endif

/* Derived (internal): true when any media transport is needed */
#if NANO_FEATURE_AUDIO || NANO_FEATURE_VIDEO
#define NANO_HAVE_MEDIA_TRANSPORT 1
#else
#define NANO_HAVE_MEDIA_TRANSPORT 0
#endif

/* ----------------------------------------------------------------
 * DataChannel limits
 * ---------------------------------------------------------------- */

#ifndef NANO_MAX_DATACHANNELS
#define NANO_MAX_DATACHANNELS 8
#endif

#ifndef NANO_DC_LABEL_SIZE
#define NANO_DC_LABEL_SIZE 32
#endif

#ifndef NANO_DC_OUT_BUF_SIZE
#define NANO_DC_OUT_BUF_SIZE 128
#endif

/* ----------------------------------------------------------------
 * ICE limits
 * ---------------------------------------------------------------- */

#ifndef NANO_MAX_ICE_CANDIDATES
#define NANO_MAX_ICE_CANDIDATES 4
#endif

#ifndef NANO_ICE_MAX_CHECKS
#define NANO_ICE_MAX_CHECKS 25
#endif

/* ICE connectivity check pacing interval in milliseconds (RFC 8445) */
#ifndef NANO_ICE_CHECK_INTERVAL_MS
#define NANO_ICE_CHECK_INTERVAL_MS 50
#endif

/* ICE credential lengths (chars, excluding NUL) */
#define NANO_ICE_UFRAG_LEN 8  /* 4 random bytes → 8 hex chars */
#define NANO_ICE_PWD_LEN   22 /* 11 random bytes → 22 hex chars */

/* ICE credential buffer sizes (must fit LEN + NUL) */

#ifndef NANO_ICE_UFRAG_SIZE
#define NANO_ICE_UFRAG_SIZE (NANO_ICE_UFRAG_LEN + 1)
#endif

#ifndef NANO_ICE_PWD_SIZE
#define NANO_ICE_PWD_SIZE (NANO_ICE_PWD_LEN + 2) /* +1 NUL +1 pad for alignment */
#endif

#ifndef NANO_ICE_REMOTE_UFRAG_SIZE
#define NANO_ICE_REMOTE_UFRAG_SIZE 32
#endif

#ifndef NANO_ICE_REMOTE_PWD_SIZE
#define NANO_ICE_REMOTE_PWD_SIZE 128
#endif

/* ----------------------------------------------------------------
 * DTLS buffer size
 * ---------------------------------------------------------------- */

#ifndef NANO_DTLS_BUF_SIZE
#define NANO_DTLS_BUF_SIZE 2048
#endif

/* ----------------------------------------------------------------
 * SDP buffer size
 * ---------------------------------------------------------------- */

#ifndef NANO_SDP_BUF_SIZE
#define NANO_SDP_BUF_SIZE 2048
#endif

/* Maximum ICE candidates parsed from a single SDP offer/answer */
#ifndef NANO_SDP_MAX_CANDIDATES
#define NANO_SDP_MAX_CANDIDATES 8
#endif

/* SDP field sizes */

#ifndef NANO_SDP_FINGERPRINT_SIZE
#define NANO_SDP_FINGERPRINT_SIZE 128
#endif

#ifndef NANO_SDP_MIN_BUF_SIZE
#define NANO_SDP_MIN_BUF_SIZE 256
#endif

/* ----------------------------------------------------------------
 * STUN scratch buffer size
 * ---------------------------------------------------------------- */

#ifndef NANO_STUN_BUF_SIZE
#define NANO_STUN_BUF_SIZE 256
#endif

/* ----------------------------------------------------------------
 * SCTP configuration (RFC 4960)
 * ---------------------------------------------------------------- */

#ifndef NANO_SCTP_SEND_BUF_SIZE
#define NANO_SCTP_SEND_BUF_SIZE 4096
#endif

#ifndef NANO_SCTP_RECV_BUF_SIZE
#define NANO_SCTP_RECV_BUF_SIZE 4096
#endif

/* Maximum SCTP packet size over DTLS */
#ifndef NANO_SCTP_MTU
#define NANO_SCTP_MTU 1200
#endif

/* Pending outbound DATA chunk queue depth */
#ifndef NANO_SCTP_MAX_SEND_QUEUE
#define NANO_SCTP_MAX_SEND_QUEUE 16
#endif

/* Maximum gap ack blocks in SACK */
#ifndef NANO_SCTP_MAX_GAP_BLOCKS
#define NANO_SCTP_MAX_GAP_BLOCKS 4
#endif

/* State cookie maximum size (bytes) */
#ifndef NANO_SCTP_COOKIE_SIZE
#define NANO_SCTP_COOKIE_SIZE 32
#endif

/* Retransmission timer defaults (RFC 4960 §6.3.1) */
#ifndef NANO_SCTP_RTO_INITIAL_MS
#define NANO_SCTP_RTO_INITIAL_MS 1000
#endif

#ifndef NANO_SCTP_RTO_MIN_MS
#define NANO_SCTP_RTO_MIN_MS 200
#endif

#ifndef NANO_SCTP_RTO_MAX_MS
#define NANO_SCTP_RTO_MAX_MS 10000
#endif

/* Maximum DATA chunk retransmissions before error */
#ifndef NANO_SCTP_MAX_RETRANSMITS
#define NANO_SCTP_MAX_RETRANSMITS 10
#endif

/* Maximum INIT retransmissions (RFC 4960 §5.1) */
#ifndef NANO_SCTP_MAX_INIT_RETRANSMITS
#define NANO_SCTP_MAX_INIT_RETRANSMITS 8
#endif

/* HEARTBEAT interval in milliseconds (RFC 4960 §8.3) */
#ifndef NANO_SCTP_HEARTBEAT_INTERVAL_MS
#define NANO_SCTP_HEARTBEAT_INTERVAL_MS 30000
#endif

/* SCTP output queue depth (number of buffered outgoing packets, power of 2) */
#ifndef NANO_SCTP_OUT_QUEUE_SIZE
#define NANO_SCTP_OUT_QUEUE_SIZE 4
#endif

/* ----------------------------------------------------------------
 * Output queue depth (must be power of 2 for ring buffer masking)
 * ---------------------------------------------------------------- */

#ifndef NANO_OUT_QUEUE_SIZE
#define NANO_OUT_QUEUE_SIZE 8
#endif

/* ----------------------------------------------------------------
 * Jitter buffer slots (AUDIO feature only)
 * ---------------------------------------------------------------- */

#ifndef NANO_JITTER_SLOTS
#define NANO_JITTER_SLOTS 64
#endif

/* ----------------------------------------------------------------
 * Logging configuration
 * ---------------------------------------------------------------- */

/**
 * @brief Maximum compile-time log level.
 *
 * Messages above this level are eliminated by the preprocessor.
 * Set via compiler flag: -DNANO_LOG_LEVEL=0 (errors only).
 * Values: 0=ERROR, 1=WARN, 2=INFO, 3=DEBUG, 4=TRACE.
 */
#ifndef NANO_LOG_LEVEL
#define NANO_LOG_LEVEL 4 /* TRACE — all messages compiled in */
#endif

/**
 * @brief Define to compile out all logging code entirely.
 */
/* #define NANO_LOG_DISABLED */

/**
 * @brief Define to omit source file/line/function from log messages.
 *
 * Reduces code size on very constrained targets.
 */
/* #define NANO_LOG_NO_LOC */

/* ----------------------------------------------------------------
 * Compile-time validation
 * ---------------------------------------------------------------- */

#if (NANO_OUT_QUEUE_SIZE & (NANO_OUT_QUEUE_SIZE - 1)) != 0
#error "NANO_OUT_QUEUE_SIZE must be a power of 2"
#endif

#if NANO_FEATURE_DATACHANNEL && NANO_MAX_DATACHANNELS < 1
#error "NANO_MAX_DATACHANNELS must be at least 1"
#endif

#if NANO_DTLS_BUF_SIZE < 256
#error "NANO_DTLS_BUF_SIZE must be at least 256"
#endif

#if NANO_ICE_UFRAG_SIZE < (NANO_ICE_UFRAG_LEN + 1)
#error "NANO_ICE_UFRAG_SIZE must be at least NANO_ICE_UFRAG_LEN + 1"
#endif

#if NANO_ICE_PWD_SIZE < (NANO_ICE_PWD_LEN + 1)
#error "NANO_ICE_PWD_SIZE must be at least NANO_ICE_PWD_LEN + 1"
#endif

#if NANO_STUN_BUF_SIZE < 128
#error "NANO_STUN_BUF_SIZE must be at least 128"
#endif

#if NANO_SDP_MIN_BUF_SIZE < 128
#error "NANO_SDP_MIN_BUF_SIZE must be at least 128"
#endif

#endif /* NANORTC_CONFIG_H_ */
