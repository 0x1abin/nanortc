/*
 * nanortc — RTP packing/unpacking internal interface (RFC 3550)
 * @internal Not part of the public API.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_RTP_H_
#define NANORTC_RTP_H_

#include <stdint.h>
#include <stddef.h>

/* RFC 3550 fixed header: V(2) P(1) X(1) CC(4) M(1) PT(7) seq(16) ts(32) SSRC(32) */
#define RTP_HEADER_SIZE 12
#define RTP_VERSION     2

/* RFC 8285 one-byte header extension profile marker. */
#define RTP_EXT_PROFILE_ONE_BYTE 0xBEDE

/* Bytes appended to each RTP packet when the TWCC extension is active:
 * 4 bytes profile+length header + 4 bytes of one 1-byte-header extension
 * (1 byte ID|len + 2 bytes seq + 1 byte pad). Kept here so callers sizing
 * scratch buffers can account for the worst case. */
#define RTP_TWCC_EXT_OVERHEAD 8

typedef struct nano_rtp {
    uint16_t seq;
    uint32_t ssrc;
    uint8_t payload_type;
    uint8_t marker; /* M bit for current/next packet */

    /* TWCC (Transport-Wide CC) RTP header extension state.
     * twcc_ext_id == 0 disables the extension (no bytes added).
     * Valid IDs are 1..14 per RFC 8285 one-byte header. */
    uint8_t twcc_ext_id;
    uint16_t twcc_seq;
} nano_rtp_t;

int rtp_init(nano_rtp_t *rtp, uint32_t ssrc, uint8_t pt);

/* Pack an RTP packet: 12-byte header + payload.
 * Increments rtp->seq. Clears marker after use. */
int rtp_pack(nano_rtp_t *rtp, uint32_t timestamp, const uint8_t *payload, size_t payload_len,
             uint8_t *buf, size_t buf_len, size_t *out_len);

/* Parse an RTP packet header and return payload pointer. */
int rtp_unpack(const uint8_t *data, size_t len, uint8_t *pt, uint16_t *seq, uint32_t *ts,
               uint32_t *ssrc, const uint8_t **payload, size_t *payload_len);

#endif /* NANORTC_RTP_H_ */
