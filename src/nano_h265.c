/*
 * nanortc — H.265/HEVC RTP packetization/depacketization (RFC 7798)
 *
 * Sans I/O: no dynamic allocation, no platform headers.
 * Packetizer uses callback pattern to emit fragments without internal buffering.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_h265.h"
#include "nano_log.h"
#include "nanortc.h"
#include <string.h>

/* Helper: extract NAL unit type from the 2-byte NAL header's byte 0. */
static inline uint8_t h265_type(uint8_t byte0)
{
    return (uint8_t)((byte0 >> 1) & H265_NAL_TYPE_MASK);
}

/* Helper: test IRAP (random access picture) NAL type — RFC 7798 §1.1.4. */
static inline int h265_is_irap(uint8_t nal_type)
{
    return (nal_type >= H265_NAL_BLA_W_LP && nal_type <= H265_NAL_CRA_NUT) ? 1 : 0;
}

/* ================================================================
 * Packetizer — Single NAL / FU (RFC 7798 §4.4.1, §4.4.3)
 * ================================================================ */

int h265_packetize(const uint8_t *nalu, size_t nalu_len, size_t mtu, h265_packet_cb cb,
                   void *userdata)
{
    if (!nalu || nalu_len < H265_NAL_HEADER_SIZE || mtu <= H265_FU_OVERHEAD || !cb) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    /* Reject re-aggregation: input NAL must not already be a FU/AP/PACI envelope
     * (RFC 7798 §4.4.2: "APs MUST NOT contain Fragmentation Units"). */
    uint8_t nal_type = h265_type(nalu[0]);
    if (nal_type == H265_NAL_FU || nal_type == H265_NAL_AP || nal_type == H265_NAL_PACI) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    /* Single NAL Unit mode (RFC 7798 §4.4.1): NAL fits in one RTP packet. */
    if (nalu_len <= mtu) {
        return cb(nalu, nalu_len, 1 /* marker: last packet of AU */, userdata);
    }

    /* FU fragmentation (RFC 7798 §4.4.3).
     *
     * Build a new 2-byte PayloadHdr that advertises type=49 (FU), but
     * preserves the F bit, LayerId, and TID from the original NAL header:
     *   byte0 = (nalu[0] & 0x81) | (49 << 1)
     *   byte1 =  nalu[1]
     *
     * The 0x81 mask keeps bit 7 (F) and bit 0 (LayerId high bit).
     * Bits 1..6 (type) are overwritten with 49. */
    uint8_t payload_hdr0 = (uint8_t)((nalu[0] & 0x81) | (uint8_t)(H265_NAL_FU << 1));
    uint8_t payload_hdr1 = nalu[1];

    /* Skip the 2-byte NAL header; its type is carried in the FU header instead. */
    const uint8_t *data = nalu + H265_NAL_HEADER_SIZE;
    size_t remaining = nalu_len - H265_NAL_HEADER_SIZE;
    size_t max_frag = mtu - H265_FU_OVERHEAD;

    int is_first = 1;
    while (remaining > 0) {
        size_t frag_len = remaining < max_frag ? remaining : max_frag;
        int is_last = (remaining == frag_len) ? 1 : 0;

        /* FU header: [S|E|FuType]. FuType is the original NAL type. */
        uint8_t fu_header = (uint8_t)(nal_type & H265_FU_TYPE_MASK);
        if (is_first) {
            fu_header |= H265_FU_S_BIT;
        }
        if (is_last) {
            fu_header |= H265_FU_E_BIT;
        }

        /* Build contiguous payload: [PayloadHdr0][PayloadHdr1][FU hdr][frag bytes]
         * The caller (RTP pack + SRTP protect) needs a single buffer. MTU is
         * bounded by NANORTC_VIDEO_MTU so a stack buffer of that size is safe. */
        uint8_t pkt[NANORTC_VIDEO_MTU];
        if (H265_FU_OVERHEAD + frag_len > sizeof(pkt)) {
            /* Defensive: should not happen if mtu ≤ NANORTC_VIDEO_MTU. */
            return NANORTC_ERR_BUFFER_TOO_SMALL;
        }

        pkt[0] = payload_hdr0;
        pkt[1] = payload_hdr1;
        pkt[2] = fu_header;
        memcpy(pkt + H265_FU_OVERHEAD, data, frag_len);

        int rc = cb(pkt, H265_FU_OVERHEAD + frag_len, is_last /* marker */, userdata);
        if (rc != 0) {
            return rc;
        }

        data += frag_len;
        remaining -= frag_len;
        is_first = 0;
    }

    return NANORTC_OK;
}

/* ================================================================
 * Depacketizer — Single NAL / AP (first sub-NAL) / FU reassembly
 * ================================================================ */

int h265_depkt_init(nano_h265_depkt_t *d)
{
    if (!d) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    memset(d, 0, sizeof(*d));
    return NANORTC_OK;
}

int h265_depkt_push(nano_h265_depkt_t *d, const uint8_t *payload, size_t len, int marker,
                    const uint8_t **nalu_out, size_t *nalu_len)
{
    (void)marker; /* Reserved: RTP marker bit for access unit boundary. */

    if (!d || !payload || len < H265_NAL_HEADER_SIZE || !nalu_out || !nalu_len) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    *nalu_out = NULL;
    *nalu_len = 0;

    uint8_t nal_type = h265_type(payload[0]);

    /* Single NAL Unit (types 0..47, RFC 7798 §4.4.1).
     * AP=48, FU=49, PACI=50 are handled below. */
    if (nal_type < H265_NAL_AP) {
        if (d->in_progress) {
            NANORTC_LOGW("H265", "FU interrupted by single NAL");
            d->in_progress = 0;
            d->len = 0;
        }

        if (len > NANORTC_VIDEO_NAL_BUF_SIZE) {
            NANORTC_LOGW("H265", "single NAL exceeds buffer");
            return NANORTC_ERR_BUFFER_TOO_SMALL;
        }
        memcpy(d->buf, payload, len);
        d->len = len;
        *nalu_out = d->buf;
        *nalu_len = len;
        return NANORTC_OK;
    }

    /* AP (type 48, RFC 7798 §4.4.2).
     *
     * Layout after the 2-byte PayloadHdr:
     *   [u16 nalu_size][nalu_size bytes]  — repeated
     *
     * TODO RFC 7798 §4.4.2: if the session advertises sprop-max-don-diff > 0,
     * DONL/DOND fields are present. We never advertise that in our SDP, so we
     * parse assuming DONL absent. Log-only mitigation for non-compliant peers.
     *
     * As with H.264 STAP-A, we return the first sub-NAL only. */
    if (nal_type == H265_NAL_AP) {
        if (d->in_progress) {
            NANORTC_LOGW("H265", "FU interrupted by AP");
            d->in_progress = 0;
            d->len = 0;
        }

        size_t offset = H265_AP_HEADER_SIZE;
        size_t buf_pos = 0;

        /* Bounds-check each length field with subtraction instead of addition
         * to avoid any chance of size_t wrap (same pattern as H.264 STAP-A). */
        while (offset + H265_AP_NALU_LEN_SIZE <= len) {
            uint16_t sub_len = (uint16_t)((uint16_t)payload[offset] << 8 | payload[offset + 1]);
            offset += H265_AP_NALU_LEN_SIZE;

            if (sub_len > len - offset) {
                NANORTC_LOGW("H265", "AP sub-NAL exceeds packet");
                return NANORTC_ERR_PARSE;
            }
            if (sub_len == 0) {
                break;
            }

            /* Return the first sub-NAL only (typically VPS/SPS/PPS or IDR). */
            if (buf_pos == 0) {
                if (sub_len > NANORTC_VIDEO_NAL_BUF_SIZE) {
                    return NANORTC_ERR_BUFFER_TOO_SMALL;
                }
                memcpy(d->buf, payload + offset, sub_len);
                buf_pos = sub_len;
            }

            offset += sub_len;
        }

        if (buf_pos > 0) {
            d->len = buf_pos;
            *nalu_out = d->buf;
            *nalu_len = buf_pos;
        }
        return NANORTC_OK;
    }

    /* FU (type 49, RFC 7798 §4.4.3) */
    if (nal_type == H265_NAL_FU) {
        if (len < H265_FU_OVERHEAD) {
            return NANORTC_ERR_PARSE;
        }

        uint8_t fu_header = payload[2];
        int is_start = (fu_header & H265_FU_S_BIT) != 0;
        int is_end = (fu_header & H265_FU_E_BIT) != 0;
        uint8_t frag_type = (uint8_t)(fu_header & H265_FU_TYPE_MASK);

        const uint8_t *frag_data = payload + H265_FU_OVERHEAD;
        size_t frag_len = len - H265_FU_OVERHEAD;

        if (is_start) {
            /* Reconstruct the original 2-byte NAL header (RFC 7798 §4.4.3):
             * keep F bit and LayerId_hi from payload[0], overwrite type with
             * frag_type; byte1 (LayerId_lo|TID) is copied verbatim. */
            d->nal_hdr0 = (uint8_t)((payload[0] & 0x81) | (uint8_t)(frag_type << 1));
            d->nal_hdr1 = payload[1];

            if (H265_NAL_HEADER_SIZE + frag_len > NANORTC_VIDEO_NAL_BUF_SIZE) {
                NANORTC_LOGW("H265", "FU start exceeds buffer");
                d->in_progress = 0;
                d->len = 0;
                return NANORTC_ERR_BUFFER_TOO_SMALL;
            }

            d->buf[0] = d->nal_hdr0;
            d->buf[1] = d->nal_hdr1;
            memcpy(d->buf + H265_NAL_HEADER_SIZE, frag_data, frag_len);
            d->len = H265_NAL_HEADER_SIZE + frag_len;
            d->in_progress = 1;
        } else if (d->in_progress) {
            if (d->len + frag_len > NANORTC_VIDEO_NAL_BUF_SIZE) {
                NANORTC_LOGW("H265", "FU reassembly exceeds buffer");
                d->in_progress = 0;
                d->len = 0;
                return NANORTC_ERR_BUFFER_TOO_SMALL;
            }
            memcpy(d->buf + d->len, frag_data, frag_len);
            d->len += frag_len;
        } else {
            /* Continuation without start — discard (lost first fragment). */
            NANORTC_LOGD("H265", "FU continuation without start");
            return NANORTC_OK;
        }

        if (is_end) {
            d->in_progress = 0;
            *nalu_out = d->buf;
            *nalu_len = d->len;
        }
        return NANORTC_OK;
    }

    /* PACI (type 50) — not supported (RFC 7798 §4.4.4). */
    NANORTC_LOGD("H265", "PACI packet ignored");
    return NANORTC_OK;
}

/* ================================================================
 * Keyframe detection (stateless)
 * ================================================================ */

int h265_is_keyframe(const uint8_t *rtp_payload, size_t len)
{
    if (!rtp_payload || len < H265_NAL_HEADER_SIZE) {
        return 0;
    }

    uint8_t nal_type = h265_type(rtp_payload[0]);

    /* Single NAL Unit (types 0..47) */
    if (nal_type < H265_NAL_AP) {
        return h265_is_irap(nal_type);
    }

    /* AP (type 48): check each sub-NAL for IRAP.
     * Use subtraction in length checks to avoid size_t wrap. */
    if (nal_type == H265_NAL_AP) {
        size_t offset = H265_AP_HEADER_SIZE;
        while (offset + H265_AP_NALU_LEN_SIZE <= len) {
            uint16_t sub_len =
                (uint16_t)((uint16_t)rtp_payload[offset] << 8 | rtp_payload[offset + 1]);
            offset += H265_AP_NALU_LEN_SIZE;
            if (sub_len == 0 || sub_len > len - offset) {
                break;
            }
            if (sub_len >= H265_NAL_HEADER_SIZE) {
                uint8_t sub_type = h265_type(rtp_payload[offset]);
                if (h265_is_irap(sub_type)) {
                    return 1;
                }
            }
            offset += sub_len;
        }
        return 0;
    }

    /* FU (type 49): only start fragment (S=1) carries reliable type info. */
    if (nal_type == H265_NAL_FU) {
        if (len < H265_FU_OVERHEAD) {
            return 0;
        }
        uint8_t fu_header = rtp_payload[2];
        if ((fu_header & H265_FU_S_BIT) == 0) {
            return 0; /* Not a start fragment. */
        }
        return h265_is_irap((uint8_t)(fu_header & H265_FU_TYPE_MASK));
    }

    return 0;
}
