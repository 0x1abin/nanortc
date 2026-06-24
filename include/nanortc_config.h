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
 * Memory profiles (approximate sizeof(nanortc_t)):
 *   Default DC-only:   ~27 KB
 *   Default DC+Audio:  ~38 KB   (per audio track: ~11 KB jitter buffer)
 *   Default full:      ~108 KB  (+ video pkt_ring + H.264 NAL buffer)
 *
 *   Minimal embedded (DC-only, no TURN):
 *     NANORTC_FEATURE_TURN 0
 *     NANORTC_MAX_DATACHANNELS 2
 *     NANORTC_MAX_ICE_CANDIDATES 4
 *     NANORTC_OUT_QUEUE_SIZE 8
 *     → ~18 KB
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

#if defined(CONFIG_NANORTC_MAX_LOCAL_CANDIDATES) && !defined(NANORTC_MAX_LOCAL_CANDIDATES)
#define NANORTC_MAX_LOCAL_CANDIDATES CONFIG_NANORTC_MAX_LOCAL_CANDIDATES
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

#if defined(CONFIG_NANORTC_SCTP_SEND_BUF_SIZE) && !defined(NANORTC_SCTP_SEND_BUF_SIZE)
#define NANORTC_SCTP_SEND_BUF_SIZE CONFIG_NANORTC_SCTP_SEND_BUF_SIZE
#endif

#if defined(CONFIG_NANORTC_SCTP_RECV_BUF_SIZE) && !defined(NANORTC_SCTP_RECV_BUF_SIZE)
#define NANORTC_SCTP_RECV_BUF_SIZE CONFIG_NANORTC_SCTP_RECV_BUF_SIZE
#endif

#if defined(CONFIG_NANORTC_SCTP_RECV_GAP_BUF_SIZE) && !defined(NANORTC_SCTP_RECV_GAP_BUF_SIZE)
#define NANORTC_SCTP_RECV_GAP_BUF_SIZE CONFIG_NANORTC_SCTP_RECV_GAP_BUF_SIZE
#endif

#if defined(CONFIG_NANORTC_SCTP_MAX_SEND_QUEUE) && !defined(NANORTC_SCTP_MAX_SEND_QUEUE)
#define NANORTC_SCTP_MAX_SEND_QUEUE CONFIG_NANORTC_SCTP_MAX_SEND_QUEUE
#endif

#if defined(CONFIG_NANORTC_SCTP_MAX_RECV_GAP) && !defined(NANORTC_SCTP_MAX_RECV_GAP)
#define NANORTC_SCTP_MAX_RECV_GAP CONFIG_NANORTC_SCTP_MAX_RECV_GAP
#endif

#if defined(CONFIG_NANORTC_LOG_LEVEL) && !defined(NANORTC_LOG_LEVEL)
#define NANORTC_LOG_LEVEL CONFIG_NANORTC_LOG_LEVEL
#endif

/* Kconfig bool → presence macro (matches src/nano_log.h `#ifdef` test). */
#if defined(CONFIG_NANORTC_LOG_NO_LOC) && !defined(NANORTC_LOG_NO_LOC)
#define NANORTC_LOG_NO_LOC
#endif

#if defined(CONFIG_NANORTC_OUT_QUEUE_SIZE) && !defined(NANORTC_OUT_QUEUE_SIZE)
#define NANORTC_OUT_QUEUE_SIZE CONFIG_NANORTC_OUT_QUEUE_SIZE
#endif

#if defined(CONFIG_NANORTC_VIDEO_PKT_RING_SIZE) && !defined(NANORTC_VIDEO_PKT_RING_SIZE)
#define NANORTC_VIDEO_PKT_RING_SIZE CONFIG_NANORTC_VIDEO_PKT_RING_SIZE
#endif

/* Send-pacing knobs (mirrored from Kconfig when the symbol exists; the
 * #ifndef defaults further down apply otherwise — including on ESP-IDF, so
 * pacing stays on by default without requiring a Kconfig entry). */
#if defined(CONFIG_NANORTC_FEATURE_VIDEO_PACING) && !defined(NANORTC_FEATURE_VIDEO_PACING)
#define NANORTC_FEATURE_VIDEO_PACING CONFIG_NANORTC_FEATURE_VIDEO_PACING
#endif
#if defined(CONFIG_NANORTC_PACING_FACTOR_PCT) && !defined(NANORTC_PACING_FACTOR_PCT)
#define NANORTC_PACING_FACTOR_PCT CONFIG_NANORTC_PACING_FACTOR_PCT
#endif
#if defined(CONFIG_NANORTC_PACING_MAX_BURST_BYTES) && !defined(NANORTC_PACING_MAX_BURST_BYTES)
#define NANORTC_PACING_MAX_BURST_BYTES CONFIG_NANORTC_PACING_MAX_BURST_BYTES
#endif
#if defined(CONFIG_NANORTC_PACING_MIN_RATE_BPS) && !defined(NANORTC_PACING_MIN_RATE_BPS)
#define NANORTC_PACING_MIN_RATE_BPS CONFIG_NANORTC_PACING_MIN_RATE_BPS
#endif
#if defined(CONFIG_NANORTC_PACING_MAX_QUEUE_MS) && !defined(NANORTC_PACING_MAX_QUEUE_MS)
#define NANORTC_PACING_MAX_QUEUE_MS CONFIG_NANORTC_PACING_MAX_QUEUE_MS
#endif
#if defined(CONFIG_NANORTC_FEATURE_VIDEO_AUTO_PLI) && !defined(NANORTC_FEATURE_VIDEO_AUTO_PLI)
#define NANORTC_FEATURE_VIDEO_AUTO_PLI CONFIG_NANORTC_FEATURE_VIDEO_AUTO_PLI
#endif
#if defined(CONFIG_NANORTC_FEATURE_VIDEO_NACK_RX) && !defined(NANORTC_FEATURE_VIDEO_NACK_RX)
#define NANORTC_FEATURE_VIDEO_NACK_RX CONFIG_NANORTC_FEATURE_VIDEO_NACK_RX
#endif
#if defined(CONFIG_NANORTC_FEATURE_VIDEO_FEC) && !defined(NANORTC_FEATURE_VIDEO_FEC)
#define NANORTC_FEATURE_VIDEO_FEC CONFIG_NANORTC_FEATURE_VIDEO_FEC
#endif
#if defined(CONFIG_NANORTC_FEC_GROUP_SIZE) && !defined(NANORTC_FEC_GROUP_SIZE)
#define NANORTC_FEC_GROUP_SIZE CONFIG_NANORTC_FEC_GROUP_SIZE
#endif
#if defined(CONFIG_NANORTC_VIDEO_PLI_MIN_INTERVAL_MS) && !defined(NANORTC_VIDEO_PLI_MIN_INTERVAL_MS)
#define NANORTC_VIDEO_PLI_MIN_INTERVAL_MS CONFIG_NANORTC_VIDEO_PLI_MIN_INTERVAL_MS
#endif
#if defined(CONFIG_NANORTC_FEATURE_VIDEO_REORDER) && !defined(NANORTC_FEATURE_VIDEO_REORDER)
#define NANORTC_FEATURE_VIDEO_REORDER CONFIG_NANORTC_FEATURE_VIDEO_REORDER
#endif
#if defined(CONFIG_NANORTC_VIDEO_REORDER_SLOTS) && !defined(NANORTC_VIDEO_REORDER_SLOTS)
#define NANORTC_VIDEO_REORDER_SLOTS CONFIG_NANORTC_VIDEO_REORDER_SLOTS
#endif
#if defined(CONFIG_NANORTC_VIDEO_REORDER_MAX_WAIT_MS) && !defined(NANORTC_VIDEO_REORDER_MAX_WAIT_MS)
#define NANORTC_VIDEO_REORDER_MAX_WAIT_MS CONFIG_NANORTC_VIDEO_REORDER_MAX_WAIT_MS
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

#if defined(CONFIG_NANORTC_VIDEO_NAL_BUF_SIZE) && !defined(NANORTC_VIDEO_NAL_BUF_SIZE)
#define NANORTC_VIDEO_NAL_BUF_SIZE CONFIG_NANORTC_VIDEO_NAL_BUF_SIZE
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

#if defined(CONFIG_NANORTC_ICE_CONSENT_INTERVAL_MS) && !defined(NANORTC_ICE_CONSENT_INTERVAL_MS)
#define NANORTC_ICE_CONSENT_INTERVAL_MS CONFIG_NANORTC_ICE_CONSENT_INTERVAL_MS
#endif

#if defined(CONFIG_NANORTC_ICE_CONSENT_TIMEOUT_MS) && !defined(NANORTC_ICE_CONSENT_TIMEOUT_MS)
#define NANORTC_ICE_CONSENT_TIMEOUT_MS CONFIG_NANORTC_ICE_CONSENT_TIMEOUT_MS
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

#if defined(IDF_VER) && !defined(NANORTC_FEATURE_TURN)
#ifdef CONFIG_NANORTC_FEATURE_TURN
#define NANORTC_FEATURE_TURN 1
#else
#define NANORTC_FEATURE_TURN 0
#endif
#endif

#if defined(IDF_VER) && !defined(NANORTC_FEATURE_IPV6)
#ifdef CONFIG_NANORTC_FEATURE_IPV6
#define NANORTC_FEATURE_IPV6 1
#else
#define NANORTC_FEATURE_IPV6 0
#endif
#endif

#if defined(IDF_VER) && !defined(NANORTC_FEATURE_H265)
#ifdef CONFIG_NANORTC_FEATURE_H265
#define NANORTC_FEATURE_H265 1
#else
#define NANORTC_FEATURE_H265 0
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
 *   NANORTC_FEATURE_IPV6         — IPv6 address parsing/generation
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

/** @brief Enable TURN relay client (RFC 5766). Default: 1.
 *  When disabled, saves ~700B RAM + ~13KB code. LAN-only deployments
 *  that do not need NAT traversal via relay can disable this. */
#ifndef NANORTC_FEATURE_TURN
#define NANORTC_FEATURE_TURN 1
#endif

/** @brief Enable IPv6 address support. Default: 1.
 *  When disabled, IPv6 candidates are silently rejected, saving ~300 bytes. */
#ifndef NANORTC_FEATURE_IPV6
#define NANORTC_FEATURE_IPV6 1
#endif

/** @brief Enable server-reflexive (srflx) candidate registration. Default: 1.
 *  STUN Binding Request/Response and trickle emission of the discovered srflx
 *  candidate happen unconditionally when a stun: server is configured. This
 *  flag controls whether the srflx candidate is also added to the local
 *  candidate set so the ICE agent can use it in connectivity checks (RFC 8445
 *  §5.1.1.2). Disable for strict LAN-only deployments where srflx pairing
 *  is never needed; the saving is small (a dedup loop + a few macros). */
#ifndef NANORTC_FEATURE_ICE_SRFLX
#define NANORTC_FEATURE_ICE_SRFLX 1
#endif

/* Derived (internal): true when any media transport is needed */
#if NANORTC_FEATURE_AUDIO || NANORTC_FEATURE_VIDEO
#define NANORTC_HAVE_MEDIA_TRANSPORT 1
#else
#define NANORTC_HAVE_MEDIA_TRANSPORT 0
#endif

/* ----------------------------------------------------------------
 * Media track limits
 * ---------------------------------------------------------------- */

/** @brief Maximum number of media tracks (audio + video combined). Default: 2. */
#ifndef NANORTC_MAX_MEDIA_TRACKS
#define NANORTC_MAX_MEDIA_TRACKS 2
#endif

/** @brief Maximum SSRC map entries for RTP demuxing. Default: 2 * MAX_MEDIA_TRACKS,
 *  plus 2 when ULPFEC is enabled: the FEC stream rides its own SSRC (media + 1)
 *  and so consumes an SRTP per-SSRC state slot (auto-created on protect/unprotect)
 *  in each direction, on top of the media SSRCs. Sized via a -D flag is seen here;
 *  a config-file-only FEC enable is caught by the validation #error below. */
#ifndef NANORTC_MAX_SSRC_MAP
#if defined(NANORTC_FEATURE_VIDEO_FEC) && NANORTC_FEATURE_VIDEO_FEC
#define NANORTC_MAX_SSRC_MAP (NANORTC_MAX_MEDIA_TRACKS * 2 + 2)
#else
#define NANORTC_MAX_SSRC_MAP (NANORTC_MAX_MEDIA_TRACKS * 2)
#endif
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
#define NANORTC_MAX_ICE_CANDIDATES 8
#endif

#ifndef NANORTC_MAX_LOCAL_CANDIDATES
#define NANORTC_MAX_LOCAL_CANDIDATES 4
#endif

#ifndef NANORTC_ICE_MAX_CHECKS
#define NANORTC_ICE_MAX_CHECKS 25
#endif

/* ICE connectivity check pacing interval in milliseconds (RFC 8445) */
#ifndef NANORTC_ICE_CHECK_INTERVAL_MS
#define NANORTC_ICE_CHECK_INTERVAL_MS 50
#endif

/* Maximum in-flight connectivity checks tracked per ICE agent (TD-018).
 * Each slot holds a txid + pair indices + timestamp so out-of-order
 * Binding Responses can be matched to the originating pair. Typical
 * browser LAN RTT < 20 ms so 4 slots at 50 ms pacing cover 200 ms of
 * in-flight budget per pair. */
#ifndef NANORTC_ICE_MAX_PENDING_CHECKS
#define NANORTC_ICE_MAX_PENDING_CHECKS 4
#endif

/* A pending connectivity check is considered stale and may be reaped
 * after this many milliseconds without a matching Binding Response. */
#ifndef NANORTC_ICE_CHECK_TIMEOUT_MS
#define NANORTC_ICE_CHECK_TIMEOUT_MS 5000
#endif

/** @brief Recommended minimum poll interval for nanortc_handle_input() (ms).
 *  Callers should tick the state machine at least this often so DTLS
 *  handshake retransmits, ICE checks, and SCTP RTO timers fire on time. */
#ifndef NANORTC_MIN_POLL_INTERVAL_MS
#define NANORTC_MIN_POLL_INTERVAL_MS 50
#endif

/* Consent freshness interval in milliseconds (RFC 7675 §5.1).
 * A STUN Binding Request is sent periodically to verify path liveness.
 * Consent expires after NANORTC_ICE_CONSENT_TIMEOUT_MS without a response. */
#ifndef NANORTC_ICE_CONSENT_INTERVAL_MS
#define NANORTC_ICE_CONSENT_INTERVAL_MS 15000
#endif

#ifndef NANORTC_ICE_CONSENT_TIMEOUT_MS
#define NANORTC_ICE_CONSENT_TIMEOUT_MS 30000
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

/** @brief Remote ICE password buffer size. Browser implementations typically use
 *  22-32 character passwords; 48 is generous for real-world interop.
 *  Override to 128 if connecting to non-standard ICE agents. */
#ifndef NANORTC_ICE_REMOTE_PWD_SIZE
#define NANORTC_ICE_REMOTE_PWD_SIZE 48
#endif

/* ----------------------------------------------------------------
 * DTLS buffer size
 * ---------------------------------------------------------------- */

#ifndef NANORTC_DTLS_BUF_SIZE
#define NANORTC_DTLS_BUF_SIZE 2048
#endif

/* Maximum ICE candidates parsed from a single SDP offer/answer */
#ifndef NANORTC_SDP_MAX_CANDIDATES
#define NANORTC_SDP_MAX_CANDIDATES 12
#endif

/* SDP field sizes */

/** @brief SDP fingerprint buffer size.
 *  SHA-256: "sha-256 " (8) + 32 hex pairs with colons (95) + NUL = 104.
 *  Reduced from 128: exact fit for SHA-256, the only hash used in WebRTC. */
#ifndef NANORTC_SDP_FINGERPRINT_SIZE
#define NANORTC_SDP_FINGERPRINT_SIZE 104
#endif

#ifndef NANORTC_SDP_MIN_BUF_SIZE
#define NANORTC_SDP_MIN_BUF_SIZE 256
#endif

/* ----------------------------------------------------------------
 * STUN / RTCP / RTP scratch buffer size
 *
 * Shared scratch buffer used for:
 *   - STUN request/response encoding (ICE checks, consent, srflx)
 *   - TURN allocate/refresh/channel framing
 *   - RTCP generation (SR/RR) and SRTCP protect
 *   - RTCP/RTP receive-side in-place unprotect
 *
 * With media transport enabled, must fit a full RTP packet including
 * CSRC list + extension header + payload + SRTP auth tag. Otherwise
 * 256 bytes is sufficient for all STUN-only use cases.
 *
 * The default is feature-gated: DC-only builds keep the small 256-byte
 * default; AUDIO/VIDEO builds get NANORTC_MEDIA_BUF_SIZE automatically.
 * Users may always override via NANORTC_CONFIG_FILE or a -D flag.
 * ---------------------------------------------------------------- */

#ifndef NANORTC_STUN_BUF_SIZE
#if NANORTC_HAVE_MEDIA_TRANSPORT
#define NANORTC_STUN_BUF_SIZE NANORTC_MEDIA_BUF_SIZE
#else
#define NANORTC_STUN_BUF_SIZE 256
#endif
#endif

/* Scratch used by lazy TURN wrap at nanortc_poll_output() time AND by the TURN
 * request builders in nano_turn.c. Must hold the larger of:
 *   - a full TURN-wrapped outbound packet: Send indication adds up to ~48 B
 *     (STUN header + XOR-PEER-ADDRESS-IPv6 + DATA attr header + padding) on top
 *     of the largest payload the application may transmit; with media transport
 *     the largest payload is a max-size SRTP packet (NANORTC_MEDIA_BUF_SIZE), so
 *     the media default sums those; and
 *   - the largest authenticated TURN request (NANORTC_TURN_MAX_REQUEST_SIZE),
 *     which dominates in non-media builds where there is no large payload.
 * The non-media default is therefore the request bound, not a bare 256 — a
 * server with a large REALM/NONCE would otherwise overflow turn_buf. The
 * #error below pins the invariant for any user override. */
#ifndef NANORTC_TURN_BUF_SIZE
#if NANORTC_HAVE_MEDIA_TRANSPORT
#define NANORTC_TURN_BUF_SIZE (NANORTC_MEDIA_BUF_SIZE + 48)
#else
#define NANORTC_TURN_BUF_SIZE NANORTC_TURN_MAX_REQUEST_SIZE
#endif
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

/* Maximum out-of-order DATA chunks buffered for gap tracking (RFC 9260 §6.2).
 * Each slot holds a copy of the payload data in recv_gap_buf. */
#ifndef NANORTC_SCTP_MAX_RECV_GAP
#define NANORTC_SCTP_MAX_RECV_GAP 8
#endif

/* Receive gap buffer size (bytes) for storing out-of-order DATA chunk payloads. */
#ifndef NANORTC_SCTP_RECV_GAP_BUF_SIZE
#define NANORTC_SCTP_RECV_GAP_BUF_SIZE 4096
#endif

/* State cookie maximum size (bytes) */
#ifndef NANORTC_SCTP_COOKIE_SIZE
#define NANORTC_SCTP_COOKIE_SIZE 32
#endif

/* Retransmission timer defaults (RFC 4960 §6.3.1) */
#ifndef NANORTC_SCTP_RTO_INITIAL_MS
#define NANORTC_SCTP_RTO_INITIAL_MS 1000
#endif

#ifndef NANORTC_SCTP_RTO_MAX_MS
#define NANORTC_SCTP_RTO_MAX_MS 10000
#endif

/* Maximum DATA chunk retransmissions before error */
#ifndef NANORTC_SCTP_MAX_RETRANSMITS
#define NANORTC_SCTP_MAX_RETRANSMITS 10
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

/* 32 slots needed for video: a single H.264 frame may produce up to ~20
 * FU-A fragments at 1200-byte MTU (for a ~24KB IDR NAL), and each fragment
 * occupies one output queue slot until dispatch. 8 slots suffice for
 * DataChannel-only or audio-only configurations.
 * For HD video (720p/1080p), override to 128 via NANORTC_CONFIG_FILE. */
#ifndef NANORTC_OUT_QUEUE_SIZE
#define NANORTC_OUT_QUEUE_SIZE 32
#endif

/* NACK retransmit ring: one SRTP-protected packet buffer per slot. Sized
 * independently from OUT_QUEUE_SIZE so IoT targets can shrink the NACK
 * window without starving the output dispatch queue. Must be a power of 2
 * and >= 4. Hard sizing rule: nanortc_send_video() emits every FU-A
 * fragment of one access unit before returning, so the caller cannot
 * drain mid-frame. PKT_RING_SIZE must be >=
 *   ceil(max_frame_bytes / NANORTC_VIDEO_MTU) + 1
 * for the worst expected access unit (typ. IDR). Wrapping the ring while
 * out_queue[].transmit.data still references older slots silently
 * corrupts the outbound bytes (stats_pkt_ring_overrun + a NANORTC_LOGW
 * fire when this happens — surfaces in integration tests rather than as
 * glitched IDRs). Default matches OUT_QUEUE_SIZE for backward compat;
 * shrinking saves
 *   (NANORTC_OUT_QUEUE_SIZE - NANORTC_VIDEO_PKT_RING_SIZE) *
 *   NANORTC_MEDIA_BUF_SIZE bytes per instance. */
#ifndef NANORTC_VIDEO_PKT_RING_SIZE
#define NANORTC_VIDEO_PKT_RING_SIZE NANORTC_OUT_QUEUE_SIZE
#endif

/** @brief NACK-answer retransmit scratch buffers (TD-023). When the sender
 *  answers a generic NACK it copies each matched pkt_ring packet into one of
 *  these and enqueues the copy, so a concurrent `nanortc_send_video()` cannot
 *  corrupt the in-flight retransmit by wrapping pkt_ring over the original slot.
 *  A NACK FCI requests up to 17 SNs; with N slots, up to N are retransmitted
 *  corruption-safely per un-drained window, the rest fall back to best-effort
 *  (the receiver re-NACKs). Each slot costs NANORTC_MEDIA_BUF_SIZE; default 4
 *  (~5 KB) — shrink for tight MCUs, 1 = a single safe retransmit at a time. */
#ifndef NANORTC_NACK_RETX_RING
#define NANORTC_NACK_RETX_RING 4
#endif

/* ----------------------------------------------------------------
 * Media transport configuration
 * ---------------------------------------------------------------- */

/** @brief RTP payload MTU for H.264 FU-A fragmentation (bytes). */
#ifndef NANORTC_VIDEO_MTU
#define NANORTC_VIDEO_MTU 1200
#endif

/* Media scratch buffer size (for RTP/SRTP processing).
 * Must hold: RTP header (12) + optional TWCC extension (8) + max payload
 * (NANORTC_VIDEO_MTU) + SRTP auth tag (10). Total overhead: 30 bytes.
 * +32 leaves 2 bytes headroom. Override to NANORTC_VIDEO_MTU + 48 if you
 * enable additional header extensions. */
#ifndef NANORTC_MEDIA_BUF_SIZE
#define NANORTC_MEDIA_BUF_SIZE (NANORTC_VIDEO_MTU + 32)
#endif

/* RTCP send interval in milliseconds (RFC 3550 §6.2) */
#ifndef NANORTC_RTCP_INTERVAL_MS
#define NANORTC_RTCP_INTERVAL_MS 5000
#endif

/* Persistent scratch for one outbound RTCP feedback packet (PLI/FIR + SRTCP
 * tag). Unlike the SR cadence (timer path, may borrow stun_buf), feedback like
 * auto-PLI is generated *during* receive processing while stun_buf holds the
 * inbound RTP packet, so it needs its own buffer whose lifetime survives until
 * nanortc_poll_output(). 64 B covers PLI (12) + SRTP auth tag + SRTCP index
 * with headroom. */
#ifndef NANORTC_RTCP_FB_BUF_SIZE
#define NANORTC_RTCP_FB_BUF_SIZE 64
#endif

/* ----------------------------------------------------------------
 * Bandwidth estimation configuration (VIDEO feature only)
 * ---------------------------------------------------------------- */

/** @brief BWE event threshold (percent). Emit NANORTC_EV_BITRATE_ESTIMATE
 *  only when the estimate changes by more than this percentage. Default: 15. */
#ifndef NANORTC_BWE_EVENT_THRESHOLD_PCT
#define NANORTC_BWE_EVENT_THRESHOLD_PCT 15
#endif

/** @brief Preferred RTP header extension ID for Transport-wide Congestion
 *  Control when nanortc is the offerer. Chrome/libdatachannel commonly
 *  advertise ID 3. Valid range: 1..14 (RFC 8285 one-byte header). When
 *  answering an offer, the offerer's ID is echoed unchanged regardless
 *  of this value. */
#ifndef NANORTC_TWCC_EXT_ID
#define NANORTC_TWCC_EXT_ID 3
#endif

/** @brief URI identifying the Transport-wide CC header extension in SDP
 *  a=extmap lines (draft-holmer-rmcat-transport-wide-cc-extensions-01). */
#ifndef NANORTC_TWCC_EXT_URI
#define NANORTC_TWCC_EXT_URI \
    "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01"
#endif

/* ----------------------------------------------------------------
 * Send pacing configuration (VIDEO feature only)
 *
 * A leaky-bucket pacer meters outbound *video* RTP egress so a large IDR
 * (tens of fragments) is spread across ~one frame interval at the
 * BWE-derived rate instead of bursting into the network bottleneck buffer
 * and causing self-inflicted loss → PLI → larger IDR. Control packets,
 * NACK retransmits and audio bypass the pacer. See src/nano_rtc_media.c
 * (pacer_enqueue / pacer_pump). The complement to the atomic frame
 * admission gate: admission decides *whether* a frame fits, pacing decides
 * *how* to spread it on the wire.
 * ---------------------------------------------------------------- */

/** @brief Enable the send-side video RTP pacer. Sub-feature of VIDEO; no
 *  effect on DataChannel / audio-only builds. When 0 the send path enqueues
 *  every fragment immediately (pre-pacer behavior — a safe fallback).
 *  Default: 1. */
#ifndef NANORTC_FEATURE_VIDEO_PACING
#define NANORTC_FEATURE_VIDEO_PACING 1
#endif

/** @brief Pacing rate as a percent of the current BWE estimate. 150 = 1.5x,
 *  giving headroom so a frame's backlog clears within a frame interval while
 *  still smoothing the burst. Must be >= 100. Default: 150. */
#ifndef NANORTC_PACING_FACTOR_PCT
#define NANORTC_PACING_FACTOR_PCT 150
#endif

/** @brief Token-bucket burst budget (bytes). The pacer may release up to
 *  this many bytes back-to-back before metering throttles egress. Bounds the
 *  instantaneous burst; must be >= NANORTC_MEDIA_BUF_SIZE so a single
 *  max-size packet can always be released. Default: 3000 (~2 MTU). */
#ifndef NANORTC_PACING_MAX_BURST_BYTES
#define NANORTC_PACING_MAX_BURST_BYTES 3000
#endif

/** @brief Pacing-rate floor (bits/sec). The pacer never meters slower than
 *  this even if the BWE estimate collapses, preventing egress stalls and
 *  divide-by-zero. Default: 100000 (100 kbps). */
#ifndef NANORTC_PACING_MIN_RATE_BPS
#define NANORTC_PACING_MIN_RATE_BPS 100000
#endif

/** @brief Hard cap on the latency the pacer may add (ms). When the oldest
 *  queued fragment has waited this long, the whole backlog drains
 *  immediately (catch-up) — pacing smooths within a frame budget but never
 *  delays across frames, preserving the real-time guarantee. ~one 25-30 fps
 *  frame. Default: 40. */
#ifndef NANORTC_PACING_MAX_QUEUE_MS
#define NANORTC_PACING_MAX_QUEUE_MS 40
#endif

/* ----------------------------------------------------------------
 * Video configuration (VIDEO feature only)
 * ---------------------------------------------------------------- */

/** @brief Maximum reassembled NAL unit size for FU-A depacketizer (bytes).
 *  16384 covers 480p H.264 IDR NAL units. Override to 32768 or 65536
 *  for 720p/1080p streams with large keyframes. */
#ifndef NANORTC_VIDEO_NAL_BUF_SIZE
#define NANORTC_VIDEO_NAL_BUF_SIZE 16384
#endif

/** @brief Default dynamic Payload Type for H.264 (RFC 6184). */
#ifndef NANORTC_VIDEO_DEFAULT_PT
#define NANORTC_VIDEO_DEFAULT_PT 96
#endif

/** @brief Auto-PLI: when the receiver detects a forward RTP sequence gap on a
 *  video track (a lost packet), automatically emit an RTCP PLI (RFC 4585
 *  §6.3.1) to request a fresh keyframe so the decoder can resync. Sub-feature
 *  of VIDEO. Without it, a receiver that loses a reference packet stays
 *  undecodable (frozen) until the application calls nanortc_request_keyframe()
 *  itself. Default: 1. */
#ifndef NANORTC_FEATURE_VIDEO_AUTO_PLI
#define NANORTC_FEATURE_VIDEO_AUTO_PLI 1
#endif

/** @brief Minimum interval between auto-PLI requests per video track (ms).
 *  Debounces keyframe requests so a burst of loss — or packet reordering,
 *  until a receive-side reorder buffer lands — costs at most one keyframe
 *  request per interval. Default: 1000. */
#ifndef NANORTC_VIDEO_PLI_MIN_INTERVAL_MS
#define NANORTC_VIDEO_PLI_MIN_INTERVAL_MS 1000
#endif

/** @brief Forward Error Correction codec (ULPFEC, RFC 5109 level-0; sub-feature
 *  of VIDEO). Proactive zero-RTT loss recovery: an XOR parity packet over a
 *  group of media packets lets the receiver reconstruct ONE lost packet per
 *  group without a retransmit. **Opt-in, default 0** — it costs constant
 *  redundancy bandwidth even with zero loss, so it only earns its place on
 *  high-RTT / high-loss links where NACK is too late. Phase 13 PR-1 ships the
 *  codec (`nano_fec.c`); SDP/RED negotiation + send/recv wiring follow. */
#ifndef NANORTC_FEATURE_VIDEO_FEC
#define NANORTC_FEATURE_VIDEO_FEC 0
#endif

/** @brief Media packets protected per FEC group (K). Overhead = 1/K. Must be
 *  1..16 (the ULPFEC level-0 16-bit mask). Default 8 (~12.5% overhead). With
 *  adaptive FEC (below) this is the MAX group / MIN overhead used at moderate
 *  loss; K is only varied downward, so the rings stay sized to this. */
#ifndef NANORTC_FEC_GROUP_SIZE
#define NANORTC_FEC_GROUP_SIZE 8
#endif

/** @brief Adaptive FEC: scale protection to the measured TWCC loss fraction
 *  (RFC 8888) instead of a fixed group size. The scarce embedded camera uplink
 *  pays *no* FEC overhead on a clean link and more protection only when it's
 *  actually lossy. Default 1 (only meaningful with FEC). K is varied in
 *  [NANORTC_FEC_MIN_GROUP .. NANORTC_FEC_GROUP_SIZE]; below the OFF threshold no
 *  FEC is sent at all. The receiver is K-agnostic (the mask carries it), so this
 *  is a sender-only, wire-compatible change. Set 0 for a fixed group size. */
#ifndef NANORTC_FEC_ADAPTIVE
#define NANORTC_FEC_ADAPTIVE 1
#endif

/** @brief Below this smoothed loss (q8, 256 = 100%), send NO FEC — clean links
 *  pay zero overhead. Default 5 (~2%), matching the BWE controller's grow band. */
#ifndef NANORTC_FEC_LOSS_OFF_Q8
#define NANORTC_FEC_LOSS_OFF_Q8 5
#endif

/** @brief At/above this smoothed loss (q8), use the smallest group (most
 *  protection). Default 25 (~10%), matching the BWE controller's shrink band. */
#ifndef NANORTC_FEC_LOSS_HIGH_Q8
#define NANORTC_FEC_LOSS_HIGH_Q8 25
#endif

/** @brief Smallest adaptive FEC group (most protection / highest overhead) used
 *  at high loss. Default = half the max group, floor 2. Must be >= 2. */
#ifndef NANORTC_FEC_MIN_GROUP
#define NANORTC_FEC_MIN_GROUP ((NANORTC_FEC_GROUP_SIZE / 2) >= 2 ? (NANORTC_FEC_GROUP_SIZE / 2) : 2)
#endif

/** @brief Number of in-flight FEC transmit buffers. A bursty frame (a large IDR
 *  that fragments into many packets) completes several FEC groups within ONE
 *  send call, before the app polls — each needs its own buffer so their pointers
 *  in out_queue don't alias. With N slots, up to N groups of an IDR get FEC;
 *  beyond that the surplus groups fall back to reorder/NACK (TD-026 L1 residual).
 *  Each slot costs NANORTC_FEC_BUF_SIZE. Default 4 (covers an IDR up to ~4·K
 *  packets fully). 1 = the original single-buffer behaviour. */
#ifndef NANORTC_FEC_TX_RING
#define NANORTC_FEC_TX_RING 4
#endif

/** @brief Dynamic RTP payload type for the outbound ULPFEC stream (RFC 5109).
 *  The FEC packets ride a separate SSRC (media SSRC + 1) so they don't pollute
 *  the media sequence space. Default 116 (dynamic range 96–127). */
#ifndef NANORTC_VIDEO_FEC_PT
#define NANORTC_VIDEO_FEC_PT 116
#endif

/** @brief Scratch size for one assembled FEC RTP packet (its 12-byte RTP header
 *  + the ULPFEC body, which can slightly exceed the largest media packet, + the
 *  SRTP tag). +64 over the media buffer covers the worst case. */
#ifndef NANORTC_FEC_BUF_SIZE
#define NANORTC_FEC_BUF_SIZE (NANORTC_MEDIA_BUF_SIZE + 64)
#endif

/** @brief Receiver-side NACK generation (sub-feature of VIDEO). On a detected
 *  forward RTP sequence gap, send an RTCP Generic NACK (RFC 4585 §6.2.1) asking
 *  the sender to retransmit the lost packet(s) from its pkt_ring history —
 *  cheaper than a keyframe for small losses, recovering within ~1 RTT. Most
 *  effective WITH the reorder buffer (which holds the gap long enough for the
 *  retransmit to arrive). **Opt-in, default 0**: it costs uplink RTCP and only
 *  helps a receiver, so a send-only camera leaves it off. */
#ifndef NANORTC_FEATURE_VIDEO_NACK_RX
#define NANORTC_FEATURE_VIDEO_NACK_RX 0
#endif

/** @brief Receive-side video reorder buffer (sub-feature of VIDEO). Holds
 *  inbound RTP packets by sequence number and releases them in order to the
 *  depacketizer, healing benign WiFi/cellular reordering before it breaks FU
 *  reassembly, and making the loss signal (auto-PLI / `contiguous`) precise —
 *  only genuine gaps, not reorders. **Opt-in, default 0**: it adds
 *  `NANORTC_VIDEO_REORDER_SLOTS × NANORTC_MEDIA_BUF_SIZE` bytes per video track
 *  and up to NANORTC_VIDEO_REORDER_MAX_WAIT_MS of receive latency, so a
 *  send-only camera should leave it off. */
#ifndef NANORTC_FEATURE_VIDEO_REORDER
#define NANORTC_FEATURE_VIDEO_REORDER 0
#endif

/** @brief Reorder window depth in packets (power of 2, >= 2). The buffer
 *  tolerates reordering up to this span; a packet farther ahead force-advances
 *  the window (declaring the skipped span lost). Default: 8. */
#ifndef NANORTC_VIDEO_REORDER_SLOTS
#define NANORTC_VIDEO_REORDER_SLOTS 8
#endif

/** @brief Max time a buffered packet waits for a missing predecessor before the
 *  gap is declared lost and skipped (ms) — the hard cap on reorder latency.
 *  Enforced on packet arrival. ~one frame at 30 fps. Default: 30. */
#ifndef NANORTC_VIDEO_REORDER_MAX_WAIT_MS
#define NANORTC_VIDEO_REORDER_MAX_WAIT_MS 30
#endif

/* ----------------------------------------------------------------
 * H.265/HEVC configuration (H265 sub-feature of VIDEO)
 *
 * RFC 7798 — "RTP Payload Format for High Efficiency Video Coding (HEVC)"
 * ---------------------------------------------------------------- */

/** @brief Enable H.265/HEVC video codec (RFC 7798). Sub-feature of VIDEO.
 *  Default: 0. Must be explicitly enabled even when NANORTC_FEATURE_VIDEO=1;
 *  H.264 is the only video codec offered by default. Adds ~11 KB of code
 *  when enabled. */
#ifndef NANORTC_FEATURE_H265
#define NANORTC_FEATURE_H265 0
#endif

#if NANORTC_FEATURE_H265 && !NANORTC_FEATURE_VIDEO
#error "NANORTC_FEATURE_H265 requires NANORTC_FEATURE_VIDEO"
#endif

/** @brief Default dynamic Payload Type for H.265 (RFC 7798).
 *  Intentionally disjoint from NANORTC_VIDEO_DEFAULT_PT (96) so a future
 *  dual-codec m-line does not need to renumber. Falls in the dynamic range
 *  96–127 per RFC 3551 §6. */
#ifndef NANORTC_VIDEO_H265_DEFAULT_PT
#define NANORTC_VIDEO_H265_DEFAULT_PT 98
#endif

/** @brief Maximum size of the pre-formatted sprop-vps/sps/pps fmtp fragment
 *  stored per H.265 m-line (bytes). Holds the already-base64-encoded
 *  parameter sets wrapped in "sprop-vps=..;sprop-sps=..;sprop-pps=.." form
 *  so SDP generation is a single append. 512 bytes covers VPS(~32B) +
 *  SPS(~128B) + PPS(~32B) after base64 expansion plus keys and separators. */
#ifndef NANORTC_H265_SPROP_FMTP_SIZE
#define NANORTC_H265_SPROP_FMTP_SIZE 512
#endif

/** @brief Maximum number of NAL units per H.265 access unit that the packer
 *  will process in a single nanortc_send_video() call. The packer reference
 *  array lives on the stack, so this also caps the stack cost. Typical VPS+
 *  SPS+PPS+SEI+IDR access units have <= 8 NALs. */
#ifndef NANORTC_MAX_NALS_PER_AU
#define NANORTC_MAX_NALS_PER_AU 16
#endif

/* ----------------------------------------------------------------
 * Jitter buffer slots (AUDIO feature only)
 * ---------------------------------------------------------------- */

/** @brief Jitter buffer ring size (number of RTP packet slots).
 *  32 slots at 20ms pacing = 640ms reorder window, sufficient for most networks.
 *  Override to 64 for high-jitter satellite or long-haul links. */
#ifndef NANORTC_JITTER_SLOTS
#define NANORTC_JITTER_SLOTS 32
#endif

/** @brief Maximum RTP payload data per jitter slot (bytes).
 *  320 covers Opus 20ms @ 128 kbps (~320 B) and G.711 20ms (160 B).
 *  Override to 640 for Opus at extreme bitrates (510 kbps). */
#ifndef NANORTC_JITTER_SLOT_DATA_SIZE
#define NANORTC_JITTER_SLOT_DATA_SIZE 320
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

#define NANORTC_ADDR_SIZE      16 /**< IPv6 binary address length (RFC 4291). */
#define NANORTC_IPV6_STR_SIZE  46 /**< Max IPv6 string length (INET6_ADDRSTRLEN). */
#define NANORTC_STUN_TXID_SIZE 12 /**< STUN transaction ID length (RFC 8489 §6). */

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
 * TURN configuration (RFC 5766)
 * ---------------------------------------------------------------- */

/** @brief Maximum TURN username length (bytes). */
#ifndef NANORTC_TURN_USERNAME_SIZE
#define NANORTC_TURN_USERNAME_SIZE 64
#endif

/** @brief Maximum TURN password/credential length (bytes). */
#ifndef NANORTC_TURN_PASSWORD_SIZE
#define NANORTC_TURN_PASSWORD_SIZE 64
#endif

/** @brief Maximum TURN realm length (bytes). */
#ifndef NANORTC_TURN_REALM_SIZE
#define NANORTC_TURN_REALM_SIZE 64
#endif

/** @brief Maximum TURN nonce length (bytes). */
#ifndef NANORTC_TURN_NONCE_SIZE
#define NANORTC_TURN_NONCE_SIZE 128
#endif

/**
 * @brief Worst-case authenticated TURN request size (bytes).
 *
 * Upper bound for any long-term-credential TURN request the client builds
 * (RFC 5766/8656). The largest is ChannelBind:
 *   STUN header + CHANNEL-NUMBER + XOR-PEER-ADDRESS(IPv6)
 *   + USERNAME + REALM + NONCE + MESSAGE-INTEGRITY.
 * USERNAME/REALM/NONCE are clamped to their storage maxima on copy-in, so this
 * is a compile-time upper bound. Each attribute is sized via
 * NANORTC_STUN_ATTR_WIRE_SIZE so the 4-byte TLV header AND the RFC 8489 §14
 * 32-bit value padding are both accounted for — budgeting the full storage
 * SIZE (rather than SIZE-1) and rounding up keeps the bound safe even when a
 * user overrides NANORTC_TURN_{USERNAME,REALM,NONCE}_SIZE to a value that is
 * not a multiple of 4. XOR-PEER-ADDRESS IPv6 value is 20 B; MESSAGE-INTEGRITY
 * is a 20-byte HMAC-SHA1. nano_turn.c's builders reject buffers smaller than
 * this, and NANORTC_TURN_BUF_SIZE must be >= it (enforced below), so the
 * builders can never overflow turn_buf even against a server with a large
 * REALM/NONCE.
 */
/* Bytes one STUN attribute occupies on the wire: 4-byte TLV header + value
 * padded up to a 32-bit boundary (RFC 8489 §14). */
#ifndef NANORTC_STUN_ATTR_WIRE_SIZE
#define NANORTC_STUN_ATTR_WIRE_SIZE(vlen) (4u + (((vlen) + 3u) & ~3u))
#endif

#ifndef NANORTC_TURN_MAX_REQUEST_SIZE
#define NANORTC_TURN_MAX_REQUEST_SIZE                                                     \
    ((8u + NANORTC_STUN_TXID_SIZE)                             /* STUN header          */ \
     + NANORTC_STUN_ATTR_WIRE_SIZE(4)                          /* CHANNEL-NUMBER       */ \
     + NANORTC_STUN_ATTR_WIRE_SIZE(20)                         /* XOR-PEER-ADDR (IPv6) */ \
     + NANORTC_STUN_ATTR_WIRE_SIZE(NANORTC_TURN_USERNAME_SIZE) /* USERNAME          */    \
     + NANORTC_STUN_ATTR_WIRE_SIZE(NANORTC_TURN_REALM_SIZE)    /* REALM             */    \
     + NANORTC_STUN_ATTR_WIRE_SIZE(NANORTC_TURN_NONCE_SIZE)    /* NONCE             */    \
     + NANORTC_STUN_ATTR_WIRE_SIZE(20))                        /* MESSAGE-INTEGRITY    */
#endif

#if NANORTC_FEATURE_TURN && (NANORTC_TURN_BUF_SIZE < NANORTC_TURN_MAX_REQUEST_SIZE)
#error \
    "NANORTC_TURN_BUF_SIZE must be >= NANORTC_TURN_MAX_REQUEST_SIZE; the largest authenticated TURN request would otherwise overflow turn_buf"
#endif

/** @brief Maximum TURN permissions (peer addresses). */
#ifndef NANORTC_TURN_MAX_PERMISSIONS
#define NANORTC_TURN_MAX_PERMISSIONS 4
#endif

/** @brief Maximum TURN channel bindings (RFC 5766 §11). */
#ifndef NANORTC_TURN_MAX_CHANNELS
#define NANORTC_TURN_MAX_CHANNELS 4
#endif

/* ----------------------------------------------------------------
 * Compile-time validation
 * ---------------------------------------------------------------- */

#if (NANORTC_OUT_QUEUE_SIZE & (NANORTC_OUT_QUEUE_SIZE - 1)) != 0
#error "NANORTC_OUT_QUEUE_SIZE must be a power of 2"
#endif

#if (NANORTC_VIDEO_PKT_RING_SIZE & (NANORTC_VIDEO_PKT_RING_SIZE - 1)) != 0
#error "NANORTC_VIDEO_PKT_RING_SIZE must be a power of 2"
#endif
#if NANORTC_VIDEO_PKT_RING_SIZE < 4
#error "NANORTC_VIDEO_PKT_RING_SIZE must be >= 4"
#endif
#if NANORTC_NACK_RETX_RING < 1 || NANORTC_NACK_RETX_RING > 64
#error "NANORTC_NACK_RETX_RING must be in 1..64 (slot index fits int8_t; bounded RAM)"
#endif

#if NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_PACING
#if NANORTC_PACING_FACTOR_PCT < 100
#error "NANORTC_PACING_FACTOR_PCT must be >= 100 (pace at least at the BWE estimate)"
#endif
#if NANORTC_PACING_MAX_BURST_BYTES < NANORTC_MEDIA_BUF_SIZE
#error \
    "NANORTC_PACING_MAX_BURST_BYTES must be >= NANORTC_MEDIA_BUF_SIZE so one max-size packet always fits the burst budget"
#endif
#if NANORTC_PACING_MAX_QUEUE_MS < 1
#error "NANORTC_PACING_MAX_QUEUE_MS must be >= 1"
#endif
#if NANORTC_PACING_MIN_RATE_BPS < 1000
#error "NANORTC_PACING_MIN_RATE_BPS must be >= 1000"
#endif
#endif /* video pacing validation */

#if NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_AUTO_PLI
#if NANORTC_VIDEO_PLI_MIN_INTERVAL_MS < 1
#error "NANORTC_VIDEO_PLI_MIN_INTERVAL_MS must be >= 1"
#endif
#endif

#if NANORTC_HAVE_MEDIA_TRANSPORT && (NANORTC_RTCP_FB_BUF_SIZE < 26)
#error "NANORTC_RTCP_FB_BUF_SIZE must be >= 26 (RTCP PLI 12 + SRTP tag 10 + SRTCP index 4)"
#endif

#if NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_FEC
#if NANORTC_FEC_GROUP_SIZE < 1 || NANORTC_FEC_GROUP_SIZE > 16
#error "NANORTC_FEC_GROUP_SIZE must be 1..16 (ULPFEC level-0 16-bit mask)"
#endif
#if NANORTC_VIDEO_FEC_PT < 96 || NANORTC_VIDEO_FEC_PT > 127
#error "NANORTC_VIDEO_FEC_PT must be a dynamic payload type (96..127)"
#endif
/* The receive path PT-intercepts the FEC stream before the SSRC->MID lookup, so
 * the FEC PT must be disjoint from every media PT or media would be misrouted. */
#if NANORTC_VIDEO_FEC_PT == NANORTC_VIDEO_DEFAULT_PT
#error "NANORTC_VIDEO_FEC_PT collides with NANORTC_VIDEO_DEFAULT_PT (H.264)"
#endif
#if NANORTC_FEATURE_H265 && (NANORTC_VIDEO_FEC_PT == NANORTC_VIDEO_H265_DEFAULT_PT)
#error "NANORTC_VIDEO_FEC_PT collides with NANORTC_VIDEO_H265_DEFAULT_PT"
#endif
/* The FEC stream's own SSRC needs an SRTP per-SSRC state slot per direction on
 * top of the media SSRCs. If FEC was enabled only via a config file (after
 * NANORTC_MAX_SSRC_MAP was already computed), bump NANORTC_MAX_SSRC_MAP. */
#if NANORTC_MAX_SSRC_MAP < (NANORTC_MAX_MEDIA_TRACKS * 2 + 2)
#error "NANORTC_FEATURE_VIDEO_FEC needs NANORTC_MAX_SSRC_MAP >= NANORTC_MAX_MEDIA_TRACKS*2 + 2"
#endif
/* FEC requires the reorder buffer. A FEC-recovered packet arrives "late" (after
 * the FEC, i.e. after later media) and must be slotted back into sequence to be
 * useful — that is exactly what the reorder buffer does. The reorder buffer is
 * also the only safe re-injection path: it copies the recovered NAL into its own
 * slot and releases one-NAL-per-poll, so the recovered + the live packet that
 * triggered recovery never alias the single per-track depacketizer buffer in one
 * handle_input (the non-reorder immediate-deliver path would). */
#if !NANORTC_FEATURE_VIDEO_REORDER
#error "NANORTC_FEATURE_VIDEO_FEC requires NANORTC_FEATURE_VIDEO_REORDER=1"
#endif
#if NANORTC_FEC_ADAPTIVE
#if NANORTC_FEC_MIN_GROUP < 2 || NANORTC_FEC_MIN_GROUP > NANORTC_FEC_GROUP_SIZE
#error "NANORTC_FEC_MIN_GROUP must be in 2..NANORTC_FEC_GROUP_SIZE"
#endif
#if NANORTC_FEC_LOSS_OFF_Q8 >= NANORTC_FEC_LOSS_HIGH_Q8
#error "NANORTC_FEC_LOSS_OFF_Q8 must be < NANORTC_FEC_LOSS_HIGH_Q8"
#endif
#endif
#if NANORTC_FEC_TX_RING < 1 || NANORTC_FEC_TX_RING > 64
#error "NANORTC_FEC_TX_RING must be in 1..64 (slot index fits int8_t; bounded RAM)"
#endif
#endif

#if NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_REORDER
#if (NANORTC_VIDEO_REORDER_SLOTS & (NANORTC_VIDEO_REORDER_SLOTS - 1)) != 0
#error "NANORTC_VIDEO_REORDER_SLOTS must be a power of 2"
#endif
#if NANORTC_VIDEO_REORDER_SLOTS < 2
#error "NANORTC_VIDEO_REORDER_SLOTS must be >= 2"
#endif
#if NANORTC_VIDEO_REORDER_MAX_WAIT_MS < 1
#error "NANORTC_VIDEO_REORDER_MAX_WAIT_MS must be >= 1"
#endif
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

/* When media transport is enabled, the shared scratch buffer is also used
 * for in-place SRTP unprotect on inbound RTP packets, so it must be large
 * enough to hold a full RTP packet (NANORTC_MEDIA_BUF_SIZE includes the
 * RTP header, CSRC, extension, payload and SRTP auth tag).
 * See the bug history in docs/exec-plans or git log for context. */
#if NANORTC_HAVE_MEDIA_TRANSPORT && (NANORTC_STUN_BUF_SIZE < NANORTC_MEDIA_BUF_SIZE)
#error \
    "NANORTC_STUN_BUF_SIZE must be >= NANORTC_MEDIA_BUF_SIZE when audio or video transport is enabled"
#endif

/* MEDIA_BUF_SIZE must fit a full outbound RTP packet:
 *   RTP header (12) + TWCC one-byte extension (8) +
 *   payload up to NANORTC_VIDEO_MTU + SRTP auth tag (10) = MTU + 30.
 * Sizes below this silently truncate outbound RTP — catch at build time.
 * TWCC overhead applies only when NANORTC_TWCC_EXT_ID is non-zero (the
 * default), but hard-coding 30 keeps the rule readable and costs only
 * 8 bytes of headroom when TWCC is compiled out. */
#if NANORTC_HAVE_MEDIA_TRANSPORT && (NANORTC_MEDIA_BUF_SIZE < NANORTC_VIDEO_MTU + 30)
#error "NANORTC_MEDIA_BUF_SIZE must be >= NANORTC_VIDEO_MTU + 30 (RTP hdr + TWCC ext + SRTP tag)"
#endif

#if NANORTC_FEATURE_DATACHANNEL
/* SCTP ring queues use `idx & (N - 1)` indexing — both depths must be a
 * power of two (and at least 2 so the mask is non-zero). Matches the
 * NANORTC_OUT_QUEUE_SIZE guard above. */
#if (NANORTC_SCTP_MAX_SEND_QUEUE < 2) || \
    ((NANORTC_SCTP_MAX_SEND_QUEUE & (NANORTC_SCTP_MAX_SEND_QUEUE - 1)) != 0)
#error "NANORTC_SCTP_MAX_SEND_QUEUE must be a power of two and at least 2"
#endif

#if (NANORTC_SCTP_MAX_RECV_GAP < 2) || \
    ((NANORTC_SCTP_MAX_RECV_GAP & (NANORTC_SCTP_MAX_RECV_GAP - 1)) != 0)
#error "NANORTC_SCTP_MAX_RECV_GAP must be a power of two and at least 2"
#endif

/* SCTP send_buf and recv_gap_buf are indexed by uint16_t offsets/lengths
 * (nsctp_send_entry_t.data_{offset,len}, send_buf_used, recv_gap_buf_used
 * in src/nano_sctp.h). Sizes above 65535 silently wrap those counters
 * on an exact-fill case — cap at build time. */
#if NANORTC_SCTP_SEND_BUF_SIZE > 65535
#error "NANORTC_SCTP_SEND_BUF_SIZE must be <= 65535 (uint16_t offsets in nsctp_send_entry_t)"
#endif

#if NANORTC_SCTP_RECV_GAP_BUF_SIZE > 65535
#error \
    "NANORTC_SCTP_RECV_GAP_BUF_SIZE must be <= 65535 (uint16_t offsets in nano_sctp_t.recv_gap_buf_used)"
#endif
#endif /* NANORTC_FEATURE_DATACHANNEL */

#if NANORTC_SDP_MIN_BUF_SIZE < 128
#error "NANORTC_SDP_MIN_BUF_SIZE must be at least 128"
#endif

#if NANORTC_MAX_LOCAL_CANDIDATES < 1
#error "NANORTC_MAX_LOCAL_CANDIDATES must be at least 1"
#endif

#if NANORTC_ICE_MAX_PENDING_CHECKS < 1
#error "NANORTC_ICE_MAX_PENDING_CHECKS must be at least 1"
#endif

#if NANORTC_ICE_MAX_PENDING_CHECKS > 16
#error "NANORTC_ICE_MAX_PENDING_CHECKS must be <= 16 (each slot uses ~20 B of nano_ice_t)"
#endif

#if NANORTC_ICE_CHECK_TIMEOUT_MS < NANORTC_ICE_CHECK_INTERVAL_MS
#error "NANORTC_ICE_CHECK_TIMEOUT_MS must be >= NANORTC_ICE_CHECK_INTERVAL_MS"
#endif

#if NANORTC_MAX_MEDIA_TRACKS < 1
#error "NANORTC_MAX_MEDIA_TRACKS must be at least 1"
#endif

#if NANORTC_MAX_SSRC_MAP < NANORTC_MAX_MEDIA_TRACKS
#error "NANORTC_MAX_SSRC_MAP must be at least NANORTC_MAX_MEDIA_TRACKS"
#endif

#endif /* NANORTC_CONFIG_H_ */
