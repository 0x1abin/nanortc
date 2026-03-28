/*
 * nanortc — RTP packing/unpacking (RFC 3550 section 5.1)
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_rtp.h"
#include "nanortc.h"
#include <string.h>

int rtp_init(nano_rtp_t *rtp, uint32_t ssrc, uint8_t pt)
{
    if (!rtp) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    memset(rtp, 0, sizeof(*rtp));
    rtp->ssrc = ssrc;
    rtp->payload_type = pt;
    rtp->marker = 1; /* M=1 on first packet (talk-spurt boundary, RFC 3550 §5.1) */
    return NANORTC_OK;
}

int rtp_pack(nano_rtp_t *rtp, uint32_t timestamp, const uint8_t *payload, size_t payload_len,
             uint8_t *buf, size_t buf_len, size_t *out_len)
{
    if (!rtp || !buf || !out_len) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    size_t total = RTP_HEADER_SIZE + payload_len;
    if (buf_len < total) {
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }

    /* RFC 3550 section 5.1: V=2, P=0, X=0, CC=0 */
    buf[0] = (RTP_VERSION << 6); /* V=2, P=0, X=0, CC=0 */
    buf[1] = (uint8_t)((rtp->marker ? 0x80 : 0) | (rtp->payload_type & 0x7F));

    nanortc_write_u16be(buf + 2, rtp->seq);
    nanortc_write_u32be(buf + 4, timestamp);
    nanortc_write_u32be(buf + 8, rtp->ssrc);

    if (payload && payload_len > 0) {
        memcpy(buf + RTP_HEADER_SIZE, payload, payload_len);
    }

    rtp->seq++;
    rtp->marker = 0;
    *out_len = total;
    return NANORTC_OK;
}

int rtp_unpack(const uint8_t *data, size_t len, uint8_t *pt, uint16_t *seq, uint32_t *ts,
               uint32_t *ssrc, const uint8_t **payload, size_t *payload_len)
{
    if (!data || len < RTP_HEADER_SIZE) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    /* Validate version == 2 */
    uint8_t version = (data[0] >> 6) & 0x03;
    if (version != RTP_VERSION) {
        return NANORTC_ERR_PARSE;
    }

    /* CSRC count extends the header */
    uint8_t cc = data[0] & 0x0F;
    size_t header_len = RTP_HEADER_SIZE + (size_t)cc * 4;

    /* Extension header (X bit) */
    if (data[0] & 0x10) {
        if (len < header_len + 4) {
            return NANORTC_ERR_PARSE;
        }
        /* Extension length in 32-bit words (RFC 3550 section 5.3.1) */
        uint16_t ext_len = nanortc_read_u16be(data + header_len + 2);
        header_len += 4 + (size_t)ext_len * 4;
    }

    if (len < header_len) {
        return NANORTC_ERR_PARSE;
    }

    if (pt) {
        *pt = data[1] & 0x7F;
    }
    if (seq) {
        *seq = nanortc_read_u16be(data + 2);
    }
    if (ts) {
        *ts = nanortc_read_u32be(data + 4);
    }
    if (ssrc) {
        *ssrc = nanortc_read_u32be(data + 8);
    }
    if (payload) {
        *payload = data + header_len;
    }
    if (payload_len) {
        *payload_len = len - header_len;
    }

    return NANORTC_OK;
}
