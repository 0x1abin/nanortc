/*
 * nanortc — RTP packing/unpacking (RFC 3550 section 5.1)
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_rtp.h"
#include "nanortc.h"
#include <stdbool.h>
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

    /* TWCC header extension (RFC 8285 §4.2, one-byte header form).
     *
     * Layout when twcc_ext_id is non-zero:
     *   bytes 0..11  : RTP fixed header (X bit set to 1)
     *   bytes 12..13 : profile = 0xBEDE
     *   bytes 14..15 : length  = 1 word of ext data
     *   byte  16     : (ID<<4) | len  where len=1 means 2 bytes of data
     *   bytes 17..18 : transport-CC sequence number (big-endian)
     *   byte  19     : zero padding to 4-byte alignment
     *
     * Extension IDs outside 1..14 are reserved by RFC 8285 and silently
     * disable the extension, so mis-negotiated values do not corrupt
     * the packet. */
    bool has_twcc = (rtp->twcc_ext_id != 0 && rtp->twcc_ext_id <= 14);
    size_t ext_bytes = has_twcc ? (size_t)RTP_TWCC_EXT_OVERHEAD : 0;

    size_t total = RTP_HEADER_SIZE + ext_bytes + payload_len;
    if (buf_len < total) {
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }

    /* RFC 3550 section 5.1: V=2, P=0, X=(ext?1:0), CC=0 */
    buf[0] = (uint8_t)((RTP_VERSION << 6) | (has_twcc ? 0x10 : 0));
    buf[1] = (uint8_t)((rtp->marker ? 0x80 : 0) | (rtp->payload_type & 0x7F));

    nanortc_write_u16be(buf + 2, rtp->seq);
    nanortc_write_u32be(buf + 4, timestamp);
    nanortc_write_u32be(buf + 8, rtp->ssrc);

    size_t off = RTP_HEADER_SIZE;

    if (has_twcc) {
        nanortc_write_u16be(buf + off, RTP_EXT_PROFILE_ONE_BYTE);
        off += 2;
        nanortc_write_u16be(buf + off, 1); /* length = 1 × 32-bit word of ext data */
        off += 2;
        buf[off++] = (uint8_t)((rtp->twcc_ext_id & 0x0F) << 4) | 0x01; /* len=1 → 2 bytes */
        nanortc_write_u16be(buf + off, rtp->twcc_seq);
        off += 2;
        buf[off++] = 0; /* pad to word boundary */
        rtp->twcc_seq++;
    }

    /* Zero-copy fast path: when the caller has already staged the payload
     * at the RTP body offset (e.g. the H.264 FU-A packetizer writing
     * directly into pkt_buf + off), the memcpy is a no-op. Comparing
     * pointers is cheap and keeps semantics byte-identical for every
     * caller that still passes a distinct payload pointer. */
    if (payload && payload_len > 0 && payload != buf + off) {
        memcpy(buf + off, payload, payload_len);
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

    /* CSRC count extends the header. cc ∈ [0..15] so cc*4 ∈ [0..60]
     * which cannot overflow size_t on any supported platform. */
    uint8_t cc = data[0] & 0x0F;
    size_t header_len = RTP_HEADER_SIZE + (size_t)cc * 4;
    if (header_len > len) {
        return NANORTC_ERR_PARSE;
    }

    /* Extension header (X bit, RFC 3550 section 5.3.1) */
    if (data[0] & 0x10) {
        size_t remaining = len - header_len;
        if (remaining < 4) {
            return NANORTC_ERR_PARSE;
        }
        uint16_t ext_len = nanortc_read_u16be(data + header_len + 2);
        /* Overflow-safe bounding: bound ext_len against the remaining
         * buffer *before* multiplying, so (ext_len * 4) cannot wrap
         * size_t on any platform (including 16-bit size_t targets).
         * (remaining - 4) / 4 is safe because remaining >= 4 above. */
        if ((size_t)ext_len > (remaining - 4) / 4) {
            return NANORTC_ERR_PARSE;
        }
        header_len += 4u + (size_t)ext_len * 4u;
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
