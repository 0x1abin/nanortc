/*
 * nanortc — Transport-Wide Congestion Control (TWCC) feedback parsing
 *
 * Parses RTCP RTPFB FMT=15 feedback packets as specified in
 * draft-holmer-rmcat-transport-wide-cc-extensions-01 §3.1.
 *
 * The parser is Sans I/O: caller hands in a buffer, receives a summary
 * and (optionally) a per-packet callback. No allocation, no I/O, no
 * timers. BWE integration is done elsewhere (see nano_bwe.*).
 *
 * @internal Not part of the public API.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_TWCC_H_
#define NANORTC_TWCC_H_

#include "nanortc_config.h"
#include <stdint.h>
#include <stddef.h>

/* RTCP PT=205 FMT=15 identifies Transport-CC feedback */
#define TWCC_FMT 15

/**
 * @brief Maximum packets reported in a single TWCC feedback packet.
 *
 * A feedback with more entries is rejected with NANORTC_ERR_PARSE.
 * The parser uses a fixed-size stack buffer sized by this macro to
 * hold per-packet statuses between the chunk-parsing and delta-parsing
 * passes.
 *
 * Default 128 covers realistic browser traffic (Chrome emits feedback
 * every ~50 ms, typical count 5–80). Tune lower on tight-stack IoT
 * devices, higher only if you observe NANORTC_ERR_PARSE in the wild.
 */
#ifndef NANORTC_TWCC_MAX_PACKETS_PER_FB
#define NANORTC_TWCC_MAX_PACKETS_PER_FB 128
#endif

/**
 * @brief Per-packet receive status in a TWCC feedback chunk.
 *
 * Matches draft §3.1.1 status symbol values. Stored as uint8_t in
 * internal arrays; exposed as enum in the public callback signature.
 */
typedef enum {
    NANO_TWCC_STATUS_NOT_RECEIVED = 0, /**< Packet lost (no delta follows). */
    NANO_TWCC_STATUS_SMALL_DELTA = 1,  /**< Received; 8-bit unsigned delta, 250 us resolution. */
    NANO_TWCC_STATUS_LARGE_DELTA = 2,  /**< Received; 16-bit signed delta, 250 us resolution. */
    NANO_TWCC_STATUS_RESERVED = 3,     /**< Reserved by RFC; treated as received for counting. */
} nano_twcc_status_t;

/**
 * @brief Summary of a single parsed TWCC feedback packet.
 *
 * Populated on successful return from twcc_parse_feedback(). All fields
 * are zeroed on entry; callers may re-use a single summary across
 * multiple feedback packets.
 */
typedef struct {
    uint32_t sender_ssrc;         /**< SSRC of the feedback sender (the media receiver). */
    uint32_t media_ssrc;          /**< SSRC of the media source the feedback reports on. */
    uint16_t base_seq;            /**< Transport-CC seq of the first reported packet. */
    uint16_t packet_status_count; /**< Number of packets reported (may include losses). */
    int64_t reference_time_us;    /**< Reference wall-clock reference, microseconds, signed. */
    uint8_t fb_pkt_count;         /**< Feedback packet counter (wraps modulo 2^8). */
    uint16_t received_count;      /**< Packets with any non-NOT_RECEIVED status. */
} nano_twcc_summary_t;

/**
 * @brief Per-packet callback fired while parsing feedback.
 *
 * Called once per reported packet, in transport-CC sequence order.
 *
 * @param seq      Transport-CC sequence number (wraps modulo 2^16).
 * @param status   Receive status (see nano_twcc_status_t).
 * @param delta_us Signed arrival delta from the previous reported
 *                 packet, in microseconds. Valid only when
 *                 @p status is SMALL_DELTA or LARGE_DELTA; 0 otherwise.
 * @param user     Opaque pointer forwarded from twcc_parse_feedback().
 */
typedef void (*nano_twcc_packet_cb_t)(uint16_t seq, nano_twcc_status_t status, int32_t delta_us,
                                      void *user);

/**
 * @brief Parse an RTCP Transport-CC feedback packet.
 *
 * Validates a full RTCP RTPFB packet (PT=205, FMT=15), extracts per-packet
 * receive status and receive deltas, and fills @p out. If @p cb is
 * non-NULL it is invoked once per reported packet in sequence order.
 *
 * @param data  Raw RTCP packet (post-SRTCP-decrypt). Must not be NULL.
 * @param len   Length of @p data in bytes.
 * @param out   Output summary. Zeroed on entry. Must not be NULL.
 * @param cb    Optional per-packet callback. Pass NULL for summary-only
 *              parsing (zero extra overhead per packet).
 * @param user  Opaque pointer forwarded to @p cb unchanged.
 *
 * @return NANORTC_OK on success, negative NANORTC_ERR_* on parse failure.
 * @retval NANORTC_ERR_INVALID_PARAM  NULL required pointer.
 * @retval NANORTC_ERR_PARSE          Malformed packet (bad version/PT/FMT,
 *                                    truncated chunks or deltas, or
 *                                    packet_status_count above
 *                                    NANORTC_TWCC_MAX_PACKETS_PER_FB).
 */
int twcc_parse_feedback(const uint8_t *data, size_t len, nano_twcc_summary_t *out,
                        nano_twcc_packet_cb_t cb, void *user);

#endif /* NANORTC_TWCC_H_ */
