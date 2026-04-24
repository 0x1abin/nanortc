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

int h264_fragment_iter_init(h264_fragment_iter_t *it, const uint8_t *nalu, size_t nalu_len,
                            size_t mtu)
{
    if (!it || !nalu || nalu_len == 0 || mtu < H264_FUA_HEADER_SIZE + 1) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    memset(it, 0, sizeof(*it));
    it->nalu = nalu;
    it->nalu_len = nalu_len;
    it->mtu = mtu;

    if (nalu_len <= mtu) {
        /* Single NAL Unit mode (RFC 6184 §5.6). */
        it->single_nal = 1;
        return NANORTC_OK;
    }

    /* FU-A fragmentation (RFC 6184 §5.8) — precompute the parts that stay
     * constant across every fragment. */
    uint8_t nal_header = nalu[0];
    uint8_t nri = nal_header & H264_NAL_REF_IDC_MASK;
    it->nal_type = nal_header & H264_NAL_TYPE_MASK;
    it->fu_indicator = nri | H264_NAL_FUA; /* F=0, NRI from original, Type=28 */
    it->data = nalu + 1;                   /* skip original NAL header byte */
    it->remaining = nalu_len - 1;
    it->max_frag = mtu - H264_FUA_HEADER_SIZE;
    return NANORTC_OK;
}

int h264_fragment_iter_has_next(const h264_fragment_iter_t *it)
{
    if (!it || it->done) {
        return 0;
    }
    return it->single_nal || it->remaining > 0;
}

int h264_fragment_iter_next(h264_fragment_iter_t *it, uint8_t *scratch, size_t scratch_len,
                            const uint8_t **payload_out, size_t *payload_len_out, int *is_last_out)
{
    if (!it || !payload_out || !payload_len_out || !is_last_out || it->done) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    if (it->single_nal) {
        *payload_out = it->nalu;
        *payload_len_out = it->nalu_len;
        *is_last_out = 1;
        it->done = 1;
        return NANORTC_OK;
    }

    if (it->remaining == 0) {
        return NANORTC_ERR_INVALID_PARAM; /* exhausted */
    }

    size_t frag_len = it->remaining < it->max_frag ? it->remaining : it->max_frag;
    if (!scratch || scratch_len < H264_FUA_HEADER_SIZE + frag_len) {
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }

    uint8_t fu_header = it->nal_type;
    if (it->data == it->nalu + 1) {
        fu_header |= H264_FUA_S_BIT; /* first fragment */
    }
    int is_last = (it->remaining == frag_len);
    if (is_last) {
        fu_header |= H264_FUA_E_BIT;
    }

    scratch[0] = it->fu_indicator;
    scratch[1] = fu_header;
    memcpy(scratch + H264_FUA_HEADER_SIZE, it->data, frag_len);

    *payload_out = scratch;
    *payload_len_out = H264_FUA_HEADER_SIZE + frag_len;
    *is_last_out = is_last;

    it->data += frag_len;
    it->remaining -= frag_len;
    if (is_last) {
        it->done = 1;
    }
    return NANORTC_OK;
}

int h264_packetize(const uint8_t *nalu, size_t nalu_len, size_t mtu, uint8_t *scratch,
                   size_t scratch_len, h264_packet_cb cb, void *userdata)
{
    if (!cb) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    h264_fragment_iter_t it;
    int rc = h264_fragment_iter_init(&it, nalu, nalu_len, mtu);
    if (rc != NANORTC_OK) {
        return rc;
    }

    while (h264_fragment_iter_has_next(&it)) {
        const uint8_t *payload = NULL;
        size_t payload_len = 0;
        int is_last = 0;
        rc = h264_fragment_iter_next(&it, scratch, scratch_len, &payload, &payload_len, &is_last);
        if (rc != NANORTC_OK) {
            return rc;
        }
        rc = cb(payload, payload_len, is_last, userdata);
        if (rc != 0) {
            return rc;
        }
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
    (void)marker; /* Reserved: RTP marker bit for access unit boundary */

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

        /* Extract all sub-NALUs concatenated into buffer.
         * Bounds-check each length field with subtraction instead of
         * addition to avoid any chance of size_t wrap (RFC 6184 §5.7). */
        while (offset + H264_STAPA_NALU_LEN_SIZE <= len) {
            uint16_t sub_len = (uint16_t)((uint16_t)payload[offset] << 8 | payload[offset + 1]);
            offset += H264_STAPA_NALU_LEN_SIZE;

            if (sub_len > len - offset) {
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

    /* STAP-A (type 24): check each sub-NAL for IDR.
     * Use subtraction in length checks to avoid any size_t wrap. */
    if (nal_type == H264_NAL_STAPA) {
        size_t offset = H264_STAPA_HEADER_SIZE;
        while (offset + H264_STAPA_NALU_LEN_SIZE <= len) {
            uint16_t sub_len =
                (uint16_t)((uint16_t)rtp_payload[offset] << 8 | rtp_payload[offset + 1]);
            offset += H264_STAPA_NALU_LEN_SIZE;
            if (sub_len == 0 || sub_len > len - offset) {
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

/* h264_annex_b_find_nal() moved to nano_annex_b.c as nano_annex_b_find_nal().
 * A #define alias in nano_h264.h preserves the legacy call-site name. */
