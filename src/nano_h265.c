/*
 * nanortc — H.265/HEVC RTP packetization/depacketization (RFC 7798)
 *
 * Sans I/O: no dynamic allocation, no platform headers.
 * All behavior strictly traces to RFC 7798 sections cited inline. This
 * module is deliberately isolated from any reference implementation
 * (libdatachannel, libwebrtc, str0m) so it can be bisect-audited against
 * the RFC text alone.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_h265.h"
#include "nanortc.h"

#include <string.h>

/* ================================================================
 * Packetizer — Single NAL / FU / AP (RFC 7798 §4.4.1, §4.4.2, §4.4.3)
 * ================================================================ */

int h265_packet_iter_init(h265_packet_iter_t *it, const h265_nal_ref_t *nals, size_t nal_count,
                          size_t mtu)
{
    if (!it || !nals || nal_count == 0 || mtu <= H265_FU_HEADER_SIZE) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    for (size_t i = 0; i < nal_count; i++) {
        if (!nals[i].data || nals[i].len < H265_NAL_HEADER_SIZE) {
            return NANORTC_ERR_INVALID_PARAM;
        }
    }
    memset(it, 0, sizeof(*it));
    it->nals = nals;
    it->nal_count = nal_count;
    it->mtu = mtu;
    return NANORTC_OK;
}

int h265_packet_iter_has_next(const h265_packet_iter_t *it)
{
    return it && it->index < it->nal_count;
}

int h265_packet_iter_next(h265_packet_iter_t *it, uint8_t *scratch, size_t scratch_len,
                          const uint8_t **payload, size_t *len, int *marker)
{
    if (!it || !payload || !len || !marker || !h265_packet_iter_has_next(it)) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    const h265_nal_ref_t *n = &it->nals[it->index];
    if (n->len > it->mtu) {
        /* RFC 7798 §4.4.3: replace the source header with PayloadHdr + FU header. */
        size_t offset = it->offset ? it->offset : H265_NAL_HEADER_SIZE;
        size_t remaining = n->len - offset;
        size_t frag = it->mtu - H265_FU_HEADER_SIZE;
        if (frag > remaining)
            frag = remaining;
        if (!scratch || frag + H265_FU_HEADER_SIZE > scratch_len)
            return NANORTC_ERR_BUFFER_TOO_SMALL;
        scratch[0] = (uint8_t)((n->data[0] & 0x81u) | (H265_PKT_FU << 1));
        scratch[1] = n->data[1];
        scratch[2] = H265_NAL_TYPE(n->data);
        if (it->offset == 0)
            scratch[2] |= H265_FU_S_BIT;
        if (frag == remaining)
            scratch[2] |= H265_FU_E_BIT;
        memcpy(scratch + H265_FU_HEADER_SIZE, n->data + offset, frag);
        *payload = scratch;
        *len = frag + H265_FU_HEADER_SIZE;
        if (frag == remaining) {
            it->index++;
            it->offset = 0;
        } else {
            it->offset = offset + frag;
        }
    } else {
        /* RFC 7798 §4.4.2: aggregate consecutive NALs when at least two fit. */
        size_t end = it->index;
        size_t bytes = H265_AP_PAYLOAD_HDR_SIZE;
        while (end < it->nal_count) {
            size_t next = it->nals[end].len;
            if (next > UINT16_MAX || next > it->mtu - bytes ||
                H265_AP_NALU_LEN_SIZE > it->mtu - bytes - next)
                break;
            bytes += H265_AP_NALU_LEN_SIZE + next;
            end++;
        }
        if (end - it->index < 2) {
            *payload = n->data;
            *len = n->len;
            it->index++;
        } else {
            if (!scratch || bytes > scratch_len)
                return NANORTC_ERR_BUFFER_TOO_SMALL;
            uint8_t layer = 63, tid = 7, f = 0;
            size_t pos = H265_AP_PAYLOAD_HDR_SIZE;
            for (size_t i = it->index; i < end; i++) {
                const h265_nal_ref_t *part = &it->nals[i];
                uint8_t part_layer = H265_NAL_LAYER_ID(part->data);
                uint8_t part_tid = H265_NAL_TID(part->data);
                if (part_layer < layer)
                    layer = part_layer;
                if (part_tid < tid)
                    tid = part_tid;
                f |= part->data[0] & H265_NAL_F_BIT;
                nanortc_write_u16be(scratch + pos, (uint16_t)part->len);
                pos += H265_AP_NALU_LEN_SIZE;
                memcpy(scratch + pos, part->data, part->len);
                pos += part->len;
            }
            if (tid == 0)
                tid = 1;
            scratch[0] = (uint8_t)(f | (H265_PKT_AP << 1) | (layer >> 5));
            scratch[1] = (uint8_t)((layer << 3) | tid);
            *payload = scratch;
            *len = pos;
            it->index = end;
        }
    }
    *marker = it->index == it->nal_count;
    return NANORTC_OK;
}

int h265_packetize_au(const h265_nal_ref_t *nals, size_t nal_count, size_t mtu, h265_packet_cb cb,
                      void *userdata)
{
    if (!cb)
        return NANORTC_ERR_INVALID_PARAM;
    h265_packet_iter_t it;
    int rc = h265_packet_iter_init(&it, nals, nal_count, mtu);
    if (rc != NANORTC_OK)
        return rc;
    uint8_t scratch[NANORTC_VIDEO_MTU];
    while (h265_packet_iter_has_next(&it)) {
        const uint8_t *payload;
        size_t len;
        int marker;
        rc = h265_packet_iter_next(&it, scratch, sizeof(scratch), &payload, &len, &marker);
        if (rc != NANORTC_OK)
            return rc;
        rc = cb(payload, len, marker, userdata);
        if (rc != NANORTC_OK)
            return rc;
    }
    return NANORTC_OK;
}

int h265_packetize(const uint8_t *nalu, size_t nalu_len, size_t mtu, h265_packet_cb cb,
                   void *userdata)
{
    const h265_nal_ref_t nal = {nalu, nalu_len};
    return h265_packetize_au(&nal, 1, mtu, cb, userdata);
}

/* ================================================================
 * Depacketizer — reassemble FU, pass-through Single, first-NAL AP
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
    (void)marker; /* Reserved — informational only */

    if (!d || !payload || len < H265_NAL_HEADER_SIZE || !nalu_out || !nalu_len) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    *nalu_out = NULL;
    *nalu_len = 0;

    uint8_t type = H265_NAL_TYPE(payload);

    /* --------------------------------------------------------------
     * Single NAL Unit Packet (RFC 7798 §4.4.1) — types 0..47.
     * The entire payload IS the NAL unit.
     * -------------------------------------------------------------- */
    if (type < H265_PKT_AP) {
        if (d->in_progress) {
            d->in_progress = 0;
            d->len = 0;
        }
        if (len > NANORTC_VIDEO_NAL_BUF_SIZE) {
            return NANORTC_ERR_BUFFER_TOO_SMALL;
        }
        memcpy(d->buf, payload, len);
        d->len = len;
        *nalu_out = d->buf;
        *nalu_len = len;
        return NANORTC_OK;
    }

    /* --------------------------------------------------------------
     * Aggregation Packet (RFC 7798 §4.4.2) — type 48.
     * Layout (sprop-max-don-diff=0 assumed — no DONL/DOND fields):
     *
     *   [PayloadHdr 2B][NALU 1 size 2B][NALU 1 ...][NALU 2 size 2B][NALU 2 ...]...
     *
     * Return the first aggregated NAL unit only, matching the H.264
     * STAP-A precedent at nano_h264.c. Subsequent aggregated NALs are
     * parsed solely to validate framing (so we error on malformed
     * input rather than over-read).
     * -------------------------------------------------------------- */
    if (type == H265_PKT_AP) {
        if (d->in_progress) {
            d->in_progress = 0;
            d->len = 0;
        }

        size_t offset = H265_AP_PAYLOAD_HDR_SIZE;
        size_t returned = 0;

        while (offset + H265_AP_NALU_LEN_SIZE <= len) {
            uint16_t sub_len =
                (uint16_t)(((uint16_t)payload[offset] << 8) | (uint16_t)payload[offset + 1]);
            offset += H265_AP_NALU_LEN_SIZE;

            /* Length bounds check via subtraction to avoid size_t wrap. */
            if (sub_len > len - offset) {
                return NANORTC_ERR_PARSE;
            }
            if (sub_len == 0) {
                break;
            }

            if (returned == 0) {
                if (sub_len > NANORTC_VIDEO_NAL_BUF_SIZE) {
                    return NANORTC_ERR_BUFFER_TOO_SMALL;
                }
                memcpy(d->buf, payload + offset, sub_len);
                returned = sub_len;
            }
            offset += sub_len;
        }

        if (returned > 0) {
            d->len = returned;
            *nalu_out = d->buf;
            *nalu_len = returned;
        }
        return NANORTC_OK;
    }

    /* --------------------------------------------------------------
     * Fragmentation Unit (RFC 7798 §4.4.3) — type 49.
     * Layout (sprop-max-don-diff=0 assumed — no DON field):
     *
     *   [PayloadHdr 2B][FU header 1B][FU payload ...]
     *
     * The PayloadHdr carries the F bit and LayerId/TID of the original
     * NAL unit; its Type is 49. The FU header carries S, E, and FuType
     * (the original nal_unit_type). The original 2-byte NAL header is
     * not present in the FU payload and must be reconstructed.
     * -------------------------------------------------------------- */
    if (type == H265_PKT_FU) {
        if (len < H265_FU_HEADER_SIZE) {
            return NANORTC_ERR_PARSE;
        }

        uint8_t fu_header = payload[H265_FU_PAYLOAD_HDR_SIZE];
        int is_start = (fu_header & H265_FU_S_BIT) != 0;
        int is_end = (fu_header & H265_FU_E_BIT) != 0;
        uint8_t fu_type = fu_header & H265_FU_TYPE_MASK;

        const uint8_t *frag_data = payload + H265_FU_HEADER_SIZE;
        size_t frag_len = len - H265_FU_HEADER_SIZE;

        if (is_start) {
            /* Reconstruct the original NAL header. The PayloadHdr already
             * carries the F bit (byte 0 bit 7), LayerId_msb (byte 0 bit 0),
             * LayerId_lsb (byte 1 bits 7..3), and TID (byte 1 bits 2..0).
             * We only need to replace the Type field (byte 0 bits 6..1,
             * currently 49) with the original FuType. */
            d->nal_hdr[0] = (uint8_t)((payload[0] & 0x81) | ((fu_type & 0x3F) << 1));
            d->nal_hdr[1] = payload[1];

            if ((size_t)H265_NAL_HEADER_SIZE + frag_len > NANORTC_VIDEO_NAL_BUF_SIZE) {
                d->in_progress = 0;
                d->len = 0;
                return NANORTC_ERR_BUFFER_TOO_SMALL;
            }
            d->buf[0] = d->nal_hdr[0];
            d->buf[1] = d->nal_hdr[1];
            memcpy(d->buf + H265_NAL_HEADER_SIZE, frag_data, frag_len);
            d->len = H265_NAL_HEADER_SIZE + frag_len;
            d->in_progress = 1;
        } else if (d->in_progress) {
            if (d->len + frag_len > NANORTC_VIDEO_NAL_BUF_SIZE) {
                d->in_progress = 0;
                d->len = 0;
                return NANORTC_ERR_BUFFER_TOO_SMALL;
            }
            memcpy(d->buf + d->len, frag_data, frag_len);
            d->len += frag_len;
        } else {
            /* FU continuation without a prior Start fragment — drop. */
            return NANORTC_OK;
        }

        if (is_end) {
            d->in_progress = 0;
            *nalu_out = d->buf;
            *nalu_len = d->len;
        }
        return NANORTC_OK;
    }

    /* --------------------------------------------------------------
     * PACI (RFC 7798 §4.4.4) — type 50. Out of scope for first pass.
     * Silently ignore, matching nano_h264.c's handling of unknown
     * NAL types.
     * -------------------------------------------------------------- */
    if (type == H265_PKT_PACI) {
        return NANORTC_OK;
    }

    /* Types 51..63 are reserved per RFC 7798 §4.4. Ignore. */
    return NANORTC_OK;
}

/* ================================================================
 * Keyframe detection — stateless, inspects the first NAL header
 * ================================================================ */

int h265_is_keyframe(const uint8_t *rtp_payload, size_t len)
{
    if (!rtp_payload || len < H265_NAL_HEADER_SIZE) {
        return 0;
    }

    uint8_t type = H265_NAL_TYPE(rtp_payload);

    /* Single NAL Unit Packet — IRAP if type is in [16..23]. */
    if (type < H265_PKT_AP) {
        return H265_IS_IRAP(type) ? 1 : 0;
    }

    /* Aggregation Packet — IRAP if any aggregated NAL is IRAP. */
    if (type == H265_PKT_AP) {
        size_t offset = H265_AP_PAYLOAD_HDR_SIZE;
        while (offset + H265_AP_NALU_LEN_SIZE <= len) {
            uint16_t sub_len = (uint16_t)(((uint16_t)rtp_payload[offset] << 8) |
                                          (uint16_t)rtp_payload[offset + 1]);
            offset += H265_AP_NALU_LEN_SIZE;
            if (sub_len == 0 || sub_len > len - offset || sub_len < H265_NAL_HEADER_SIZE) {
                break;
            }
            uint8_t sub_type = H265_NAL_TYPE(&rtp_payload[offset]);
            if (H265_IS_IRAP(sub_type)) {
                return 1;
            }
            offset += sub_len;
        }
        return 0;
    }

    /* Fragmentation Unit — only start fragments carry the original type. */
    if (type == H265_PKT_FU) {
        if (len < H265_FU_HEADER_SIZE) {
            return 0;
        }
        uint8_t fu_header = rtp_payload[H265_FU_PAYLOAD_HDR_SIZE];
        if ((fu_header & H265_FU_S_BIT) == 0) {
            return 0;
        }
        uint8_t fu_type = fu_header & H265_FU_TYPE_MASK;
        return H265_IS_IRAP(fu_type) ? 1 : 0;
    }

    /* PACI and reserved types — not keyframe by definition. */
    return 0;
}
