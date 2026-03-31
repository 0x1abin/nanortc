/*
 * nanortc — H.264 RTP packetization/depacketization (RFC 6184)
 *
 * Sans I/O: no dynamic allocation, no platform headers.
 * Packetizer uses callback pattern to emit fragments without internal buffering.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_h264.h"
#include "nano_log.h"
#include "nanortc.h"
#include <string.h>

/* ================================================================
 * Packetizer — Single NAL / FU-A (RFC 6184 §5.6, §5.8)
 * ================================================================ */

int h264_packetize(const uint8_t *nalu, size_t nalu_len, size_t mtu, h264_packet_cb cb,
                   void *userdata)
{
    if (!nalu || nalu_len == 0 || mtu < H264_FUA_HEADER_SIZE + 1 || !cb) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    /* Single NAL Unit mode (RFC 6184 §5.6): NAL fits in one RTP packet */
    if (nalu_len <= mtu) {
        return cb(nalu, nalu_len, 1 /* marker: last packet */, userdata);
    }

    /* FU-A fragmentation (RFC 6184 §5.8) */
    uint8_t nal_header = nalu[0];
    uint8_t nri = nal_header & H264_NAL_REF_IDC_MASK;   /* NRI bits */
    uint8_t nal_type = nal_header & H264_NAL_TYPE_MASK; /* NAL unit type */

    /* FU indicator: F=0, NRI from original, Type=28 (FU-A) */
    uint8_t fu_indicator = nri | H264_NAL_FUA;

    /* Skip the first NAL header byte — it's reconstructed from FU indicator + FU header */
    const uint8_t *data = nalu + 1;
    size_t remaining = nalu_len - 1;
    size_t max_frag = mtu - H264_FUA_HEADER_SIZE;

    while (remaining > 0) {
        size_t frag_len = remaining < max_frag ? remaining : max_frag;
        uint8_t fu_header = nal_type;

        if (data == nalu + 1) {
            /* First fragment: set Start bit (RFC 6184 §5.8) */
            fu_header |= H264_FUA_S_BIT;
        }
        if (remaining == frag_len) {
            /* Last fragment: set End bit */
            fu_header |= H264_FUA_E_BIT;
        }

        /* Build FU-A payload: [FU indicator][FU header][fragment data]
         * We need a contiguous buffer. Use a stack-local header + callback
         * with the fragment data. To avoid requiring a scratch buffer for
         * the full payload, we build a small header and rely on the callback
         * being able to handle a 2-part payload. However, our API specifies
         * a single contiguous payload, so we must provide one.
         *
         * Since the caller's media_buf is large enough (NANORTC_MEDIA_BUF_SIZE),
         * and the callback will copy into an RTP packet, we can use a small
         * stack buffer for the FU-A header and pass the original data pointer.
         *
         * Alternative: build the payload in a stack buffer up to MTU size.
         * MTU is typically 1200, which is safe for stack. */
        uint8_t pkt[NANORTC_VIDEO_MTU];
        if (H264_FUA_HEADER_SIZE + frag_len > sizeof(pkt)) {
            /* Safety: should not happen if mtu <= NANORTC_VIDEO_MTU */
            return NANORTC_ERR_BUFFER_TOO_SMALL;
        }

        pkt[0] = fu_indicator;
        pkt[1] = fu_header;
        memcpy(pkt + H264_FUA_HEADER_SIZE, data, frag_len);

        int marker = (remaining == frag_len) ? 1 : 0;
        int rc = cb(pkt, H264_FUA_HEADER_SIZE + frag_len, marker, userdata);
        if (rc != 0) {
            return rc;
        }

        data += frag_len;
        remaining -= frag_len;
    }

    return NANORTC_OK;
}

/* ================================================================
 * Depacketizer — reassemble FU-A / pass-through Single NAL / STAP-A
 * ================================================================ */

int h264_depkt_init(nano_h264_depkt_t *d)
{
    if (!d) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    memset(d, 0, sizeof(*d));
    return NANORTC_OK;
}

int h264_depkt_push(nano_h264_depkt_t *d, const uint8_t *payload, size_t len, int marker,
                    const uint8_t **nalu_out, size_t *nalu_len)
{
    if (!d || !payload || len == 0 || !nalu_out || !nalu_len) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    *nalu_out = NULL;
    *nalu_len = 0;

    uint8_t nal_type = payload[0] & H264_NAL_TYPE_MASK;

    /* Single NAL Unit (types 1-23, RFC 6184 §5.6) */
    if (nal_type >= 1 && nal_type <= 23) {
        /* Abort any in-progress FU-A reassembly */
        if (d->in_progress) {
            NANORTC_LOGW("H264", "FU-A interrupted by single NAL");
            d->in_progress = 0;
            d->len = 0;
        }

        /* Copy into buffer and return */
        if (len > NANORTC_VIDEO_NAL_BUF_SIZE) {
            NANORTC_LOGW("H264", "single NAL exceeds buffer");
            return NANORTC_ERR_BUFFER_TOO_SMALL;
        }
        memcpy(d->buf, payload, len);
        d->len = len;
        *nalu_out = d->buf;
        *nalu_len = len;
        return NANORTC_OK;
    }

    /* STAP-A (type 24, RFC 6184 §5.7) — return first NAL only for simplicity.
     * Browser may send SPS+PPS+IDR as STAP-A. We extract and return
     * each sub-NAL by repeated calls (caller should check marker bit).
     * For now, extract the first sub-NAL unit. */
    if (nal_type == H264_NAL_STAPA) {
        if (d->in_progress) {
            NANORTC_LOGW("H264", "FU-A interrupted by STAP-A");
            d->in_progress = 0;
            d->len = 0;
        }

        size_t offset = H264_STAPA_HEADER_SIZE;
        size_t buf_pos = 0;

        /* Extract all sub-NALUs concatenated into buffer */
        while (offset + H264_STAPA_NALU_LEN_SIZE <= len) {
            uint16_t sub_len = (uint16_t)((uint16_t)payload[offset] << 8 | payload[offset + 1]);
            offset += H264_STAPA_NALU_LEN_SIZE;

            if (offset + sub_len > len) {
                NANORTC_LOGW("H264", "STAP-A sub-NAL exceeds packet");
                return NANORTC_ERR_PARSE;
            }
            if (sub_len == 0) {
                break;
            }

            /* Return the first sub-NAL only (most useful: typically SPS/PPS/IDR) */
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

    /* FU-A (type 28, RFC 6184 §5.8) */
    if (nal_type == H264_NAL_FUA) {
        if (len < H264_FUA_HEADER_SIZE) {
            return NANORTC_ERR_PARSE;
        }

        uint8_t fu_header = payload[1];
        int is_start = (fu_header & H264_FUA_S_BIT) != 0;
        int is_end = (fu_header & H264_FUA_E_BIT) != 0;
        uint8_t frag_type = fu_header & H264_NAL_TYPE_MASK;
        uint8_t nri = payload[0] & H264_NAL_REF_IDC_MASK;

        const uint8_t *frag_data = payload + H264_FUA_HEADER_SIZE;
        size_t frag_len = len - H264_FUA_HEADER_SIZE;

        if (is_start) {
            /* Start of new NAL: reconstruct NAL header byte */
            d->nal_header = nri | frag_type;
            d->buf[0] = d->nal_header;
            d->len = 1;
            d->in_progress = 1;

            if (1 + frag_len > NANORTC_VIDEO_NAL_BUF_SIZE) {
                NANORTC_LOGW("H264", "FU-A start exceeds buffer");
                d->in_progress = 0;
                d->len = 0;
                return NANORTC_ERR_BUFFER_TOO_SMALL;
            }
            memcpy(d->buf + 1, frag_data, frag_len);
            d->len = 1 + frag_len;
        } else if (d->in_progress) {
            /* Continuation or end fragment */
            if (d->len + frag_len > NANORTC_VIDEO_NAL_BUF_SIZE) {
                NANORTC_LOGW("H264", "FU-A reassembly exceeds buffer");
                d->in_progress = 0;
                d->len = 0;
                return NANORTC_ERR_BUFFER_TOO_SMALL;
            }
            memcpy(d->buf + d->len, frag_data, frag_len);
            d->len += frag_len;
        } else {
            /* Continuation without start — discard */
            NANORTC_LOGD("H264", "FU-A continuation without start");
            return NANORTC_OK;
        }

        if (is_end) {
            /* Reassembly complete */
            d->in_progress = 0;
            *nalu_out = d->buf;
            *nalu_len = d->len;
        }

        return NANORTC_OK;
    }

    /* Unknown NAL type — ignore */
    NANORTC_LOGD("H264", "unknown NAL type in RTP payload");
    return NANORTC_OK;
}

/* ================================================================
 * Keyframe detection (stateless)
 * ================================================================ */

int h264_is_keyframe(const uint8_t *rtp_payload, size_t len)
{
    if (!rtp_payload || len == 0) {
        return 0;
    }

    uint8_t nal_type = rtp_payload[0] & H264_NAL_TYPE_MASK;

    /* Single NAL unit (types 1-23): keyframe = IDR (type 5) */
    if (nal_type >= 1 && nal_type <= 23) {
        return (nal_type == H264_NAL_IDR) ? 1 : 0;
    }

    /* STAP-A (type 24): check each sub-NAL for IDR */
    if (nal_type == H264_NAL_STAPA) {
        size_t offset = H264_STAPA_HEADER_SIZE;
        while (offset + H264_STAPA_NALU_LEN_SIZE <= len) {
            uint16_t sub_len =
                (uint16_t)((uint16_t)rtp_payload[offset] << 8 | rtp_payload[offset + 1]);
            offset += H264_STAPA_NALU_LEN_SIZE;
            if (offset + sub_len > len || sub_len == 0) {
                break;
            }
            if ((rtp_payload[offset] & H264_NAL_TYPE_MASK) == H264_NAL_IDR) {
                return 1;
            }
            offset += sub_len;
        }
        return 0;
    }

    /* FU-A (type 28): only start fragment (S=1) carries reliable type info */
    if (nal_type == H264_NAL_FUA) {
        if (len < H264_FUA_HEADER_SIZE) {
            return 0;
        }
        uint8_t fu_header = rtp_payload[1];
        if ((fu_header & H264_FUA_S_BIT) == 0) {
            return 0; /* Not a start fragment */
        }
        return ((fu_header & H264_NAL_TYPE_MASK) == H264_NAL_IDR) ? 1 : 0;
    }

    return 0;
}

const uint8_t *h264_annex_b_find_nal(const uint8_t *data, size_t len, size_t *offset,
                                     size_t *nal_len)
{
    if (!data || !offset || !nal_len) {
        return NULL;
    }

    size_t i = *offset;

    /* Scan for start code: 00 00 01 or 00 00 00 01 */
    while (i + 2 < len) {
        if (data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) {
                i += 3;
                break;
            }
            if (i + 3 < len && data[i + 2] == 0 && data[i + 3] == 1) {
                i += 4;
                break;
            }
        }
        i++;
    }

    if (i >= len) {
        return NULL;
    }

    const uint8_t *nal_start = data + i;

    /* Find next start code or end of buffer */
    size_t j = i;
    while (j + 2 < len) {
        if (data[j] == 0 && data[j + 1] == 0 &&
            (data[j + 2] == 1 || (j + 3 < len && data[j + 2] == 0 && data[j + 3] == 1))) {
            break;
        }
        j++;
    }
    if (j + 2 >= len) {
        j = len;
    }

    /* Strip trailing zero padding between NALs */
    size_t end = j;
    while (end > i && data[end - 1] == 0) {
        end--;
    }

    *nal_len = end - i;
    *offset = j;
    return nal_start;
}
