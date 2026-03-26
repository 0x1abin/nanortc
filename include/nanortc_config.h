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

/* ----------------------------------------------------------------
 * Build profile
 * ---------------------------------------------------------------- */

#ifndef NANORTC_PROFILE
#define NANORTC_PROFILE 1 /* NANO_PROFILE_DATA */
#endif

/* ----------------------------------------------------------------
 * DataChannel limits
 * ---------------------------------------------------------------- */

#ifndef NANO_MAX_DATACHANNELS
#define NANO_MAX_DATACHANNELS 8
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

/* ----------------------------------------------------------------
 * SCTP buffer sizes
 * ---------------------------------------------------------------- */

#ifndef NANO_SCTP_SEND_BUF_SIZE
#define NANO_SCTP_SEND_BUF_SIZE (64 * 1024)
#endif

#ifndef NANO_SCTP_RECV_BUF_SIZE
#define NANO_SCTP_RECV_BUF_SIZE (64 * 1024)
#endif

/* ----------------------------------------------------------------
 * Output queue depth (must be power of 2 for ring buffer masking)
 * ---------------------------------------------------------------- */

#ifndef NANO_OUT_QUEUE_SIZE
#define NANO_OUT_QUEUE_SIZE 8
#endif

/* ----------------------------------------------------------------
 * Jitter buffer slots (AUDIO/MEDIA profiles only)
 * ---------------------------------------------------------------- */

#ifndef NANO_JITTER_SLOTS
#define NANO_JITTER_SLOTS 64
#endif

/* ----------------------------------------------------------------
 * Compile-time validation
 * ---------------------------------------------------------------- */

#if (NANO_OUT_QUEUE_SIZE & (NANO_OUT_QUEUE_SIZE - 1)) != 0
#error "NANO_OUT_QUEUE_SIZE must be a power of 2"
#endif

#if NANO_MAX_DATACHANNELS < 1
#error "NANO_MAX_DATACHANNELS must be at least 1"
#endif

#if NANO_DTLS_BUF_SIZE < 256
#error "NANO_DTLS_BUF_SIZE must be at least 256"
#endif

#endif /* NANORTC_CONFIG_H_ */
