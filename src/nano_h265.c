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
#include "nano_log.h"
#include "nanortc.h"

#include <string.h>

/* ================================================================
 * Packetizer — Single NAL / FU / AP (RFC 7798 §4.4.1, §4.4.2, §4.4.3)
 * ================================================================ */

/**
 * Emit a single Single NAL Unit Packet (RFC 7798 §4.4.1).
 *
 * Per §4.4.1: "The RTP payload [...] MUST contain a single NAL unit."
 * There is no RTP-level header added on top of the NAL unit in Single NAL
 * mode — the NAL header bytes are the first bytes of the RTP payload.
 */
static int h265_emit_single(const uint8_t *nalu, size_t nalu_len, int marker, h265_packet_cb cb,
                            void *userdata)
{
    return cb(nalu, nalu_len, marker, userdata);
}

/**
 * Emit a Fragmentation Unit sequence (RFC 7798 §4.4.3).
 *
 * Wire format:
 *
 *    0                   1                   2                   3
 *    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *   +---------------+---------------+---------------+---------------+
 *   | PayloadHdr (Type=49)          |   FU header   |   ...         |
 *   +---------------+---------------+---------------+               |
 *   |                                                               |
 *   |                     FU payload                                |
 *   |                                                               |
 *   |                               +---------------+---------------+
 *   |                               :   OPT RTP padding             |
 *   +---------------+---------------+---------------+---------------+
 *
 * Per §4.4.3:
 *   - The PayloadHdr carries the F bit from the original NAL, Type = 49,
 *     and LayerId / TID copied from the fragmented NAL's header.
 *   - The FU header carries Start (S), End (E), and FuType (original NAL
 *     unit type) fields.
 *   - The original 2-byte NAL header is NOT included in the FU payload —
 *     it is reconstructed at the receiver from PayloadHdr + FU header.
 *   - DON field is present only when sprop-max-don-diff > 0. First pass
 *     assumes 0, so DON is never emitted.
 */
static int h265_emit_fu(const uint8_t *nalu, size_t nalu_len, size_t mtu, int marker_last_au,
                        h265_packet_cb cb, void *userdata)
{
    if (nalu_len <= H265_NAL_HEADER_SIZE) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (mtu <= H265_FU_HEADER_SIZE) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (mtu > NANORTC_VIDEO_MTU) {
        /* Caller sized MTU above our stack scratch buffer — refuse. */
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }

    /* Extract original NAL fields */
    uint8_t fu_type = H265_NAL_TYPE(nalu);

    /* Build the PayloadHdr by cloning the original NAL header and
     * overwriting only the Type field with 49.
     *
     * byte[0] = F(1) | Type(6) | LayerId_msb(1)
     * byte[1] = LayerId_lsb(5) | TID(3)
     *
     * Mask 0x81 preserves F bit (0x80) and LayerId MSB (0x01) in byte 0.
     * Type is placed in bits 6..1 via (49 << 1) == 0x62.
     */
    uint8_t payload_hdr0 = (uint8_t)((nalu[0] & 0x81) | (H265_PKT_FU << 1));
    uint8_t payload_hdr1 = nalu[1];

    /* Data pointer and remaining count skip the NAL header bytes. */
    const uint8_t *data = nalu + H265_NAL_HEADER_SIZE;
    size_t remaining = nalu_len - H265_NAL_HEADER_SIZE;
    size_t max_frag = mtu - H265_FU_HEADER_SIZE;

    /* Stack scratch buffer for the FU RTP payload. NANORTC_VIDEO_MTU caps
     * the size and matches the per-packet limit used by the H.264 path. */
    uint8_t pkt[NANORTC_VIDEO_MTU];

    int first_frag = 1;
    while (remaining > 0) {
        size_t frag_len = remaining < max_frag ? remaining : max_frag;
        int is_last_frag = (remaining == frag_len);

        /* FU header: S bit on the first fragment, E bit on the last. */
        uint8_t fu_header = fu_type;
        if (first_frag) {
            fu_header |= H265_FU_S_BIT;
        }
        if (is_last_frag) {
            fu_header |= H265_FU_E_BIT;
        }

        pkt[0] = payload_hdr0;
        pkt[1] = payload_hdr1;
        pkt[2] = fu_header;
        memcpy(pkt + H265_FU_HEADER_SIZE, data, frag_len);

        /* Marker bit: only on the last fragment AND only if this NAL is
         * the final NAL in the access unit. */
        int marker = (is_last_frag && marker_last_au) ? 1 : 0;
        int rc = cb(pkt, H265_FU_HEADER_SIZE + frag_len, marker, userdata);
        if (rc != 0) {
            return rc;
        }

        data += frag_len;
        remaining -= frag_len;
        first_frag = 0;
    }

    return NANORTC_OK;
}

/**
 * Emit one Aggregation Packet (RFC 7798 §4.4.2).
 *
 * Wire format (for a run of N aggregated NAL units, sprop-max-don-diff=0):
 *
 *    0                   1                   2                   3
 *    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *   +---------------+---------------+---------------+---------------+
 *   | PayloadHdr (Type=48)          |         NALU 1 Size           |
 *   +---------------+---------------+---------------+---------------+
 *   |         NALU 1 HDR            |                               |
 *   +---------------+---------------+  NALU 1 Data                  |
 *   :                              ...                              :
 *   |                               +---------------+---------------+
 *   :                               |         NALU 2 Size           |
 *   +---------------+---------------+---------------+---------------+
 *   |         NALU 2 HDR            |                               |
 *   +---------------+---------------+  NALU 2 Data                  |
 *   :                              ...                              :
 *
 * Per §4.4.2 header-field rules for the PayloadHdr:
 *   - F bit MUST be 1 iff any aggregated NAL has F=1; otherwise 0.
 *   - LayerId MUST be the smallest LayerId of the aggregated NALs.
 *   - TID (nuh_temporal_id_plus1) MUST be the smallest of the aggregated
 *     NALs' TIDs (i.e., the smallest temporal layer).
 */
static int h265_emit_ap(const h265_nal_ref_t *nals, size_t start, size_t count, int marker,
                        h265_packet_cb cb, void *userdata)
{
    if (count < 2) {
        /* An AP with a single NAL is spec-legal but wastes bytes; the
         * caller should use a Single NAL Unit Packet instead. */
        return NANORTC_ERR_INVALID_PARAM;
    }

    /* Scan aggregated NALs for min LayerId / min TID / max F bit. */
    uint8_t min_layer_id = 63;
    uint8_t min_tid = 7;
    uint8_t f_bit = 0;
    for (size_t i = 0; i < count; ++i) {
        const h265_nal_ref_t *n = &nals[start + i];
        if (n->len < H265_NAL_HEADER_SIZE) {
            return NANORTC_ERR_INVALID_PARAM;
        }
        uint8_t lid = H265_NAL_LAYER_ID(n->data);
        uint8_t tid = H265_NAL_TID(n->data);
        if (n->data[0] & H265_NAL_F_BIT) {
            f_bit = 1;
        }
        if (lid < min_layer_id) {
            min_layer_id = lid;
        }
        if (tid < min_tid) {
            min_tid = tid;
        }
    }

    /* TID MUST be > 0 per H.265 §7.4.2.2 nuh_temporal_id_plus1; if all
     * aggregated NALs were malformed with TID=0 we would have bailed at
     * the nal_len check above. Clamp to 1 as a defensive floor. */
    if (min_tid == 0) {
        min_tid = 1;
    }

    uint8_t pkt[NANORTC_VIDEO_MTU];
    if (H265_AP_PAYLOAD_HDR_SIZE > sizeof(pkt)) {
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }

    /* Build PayloadHdr:
     *   byte[0] = F(1) | Type=48(6) | LayerId_msb(1)
     *   byte[1] = LayerId_lsb(5) | TID(3)
     */
    pkt[0] = (uint8_t)((f_bit ? H265_NAL_F_BIT : 0) | ((H265_PKT_AP & 0x3F) << 1) |
                       ((min_layer_id >> 5) & 0x01));
    pkt[1] = (uint8_t)(((min_layer_id & 0x1F) << 3) | (min_tid & 0x07));

    size_t pos = H265_AP_PAYLOAD_HDR_SIZE;
    for (size_t i = 0; i < count; ++i) {
        const h265_nal_ref_t *n = &nals[start + i];
        /* Defensive bound check — caller (h265_packetize_au) already
         * guaranteed it fits, but we re-check so the module is safe to
         * call standalone in tests and fuzz. */
        if (pos + H265_AP_NALU_LEN_SIZE + n->len > sizeof(pkt)) {
            return NANORTC_ERR_BUFFER_TOO_SMALL;
        }
        pkt[pos + 0] = (uint8_t)((n->len >> 8) & 0xFF);
        pkt[pos + 1] = (uint8_t)(n->len & 0xFF);
        pos += H265_AP_NALU_LEN_SIZE;
        memcpy(pkt + pos, n->data, n->len);
        pos += n->len;
    }

    return cb(pkt, pos, marker ? 1 : 0, userdata);
}

int h265_packetize_au(const h265_nal_ref_t *nals, size_t nal_count, size_t mtu, h265_packet_cb cb,
                      void *userdata)
{
    if (!nals || nal_count == 0 || mtu < H265_FU_HEADER_SIZE + 1 || !cb) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    size_t i = 0;
    while (i < nal_count) {
        const h265_nal_ref_t *n = &nals[i];
        if (!n->data || n->len < H265_NAL_HEADER_SIZE) {
            return NANORTC_ERR_INVALID_PARAM;
        }

        /* Case 1: NAL larger than a Single NAL Unit Packet → FU (§4.4.3). */
        if (n->len > mtu) {
            int is_last = (i + 1 == nal_count) ? 1 : 0;
            int rc = h265_emit_fu(n->data, n->len, mtu, is_last, cb, userdata);
            if (rc != 0) {
                return rc;
            }
            i++;
            continue;
        }

        /* Case 2: Try to aggregate current and following NALs into an AP
         * (§4.4.2). An AP is only profitable when aggregating 2+ NALs;
         * for a single NAL we fall through to Single NAL Unit Packet. */
        size_t ap_bytes = H265_AP_PAYLOAD_HDR_SIZE + H265_AP_NALU_LEN_SIZE + n->len;
        size_t j = i;
        while (j + 1 < nal_count) {
            const h265_nal_ref_t *next = &nals[j + 1];
            if (!next->data || next->len < H265_NAL_HEADER_SIZE) {
                /* Malformed tail — stop the run; outer loop will fail on it. */
                break;
            }
            /* A large NAL that needs FU cannot be mixed into an AP —
             * stop the run so the next iteration handles it via case 1. */
            if (next->len > mtu) {
                break;
            }
            size_t next_bytes = H265_AP_NALU_LEN_SIZE + next->len;
            if (ap_bytes + next_bytes > mtu) {
                break;
            }
            ap_bytes += next_bytes;
            j++;
        }

        if (j > i) {
            int is_last = (j + 1 == nal_count) ? 1 : 0;
            int rc = h265_emit_ap(nals, i, j - i + 1, is_last, cb, userdata);
            if (rc != 0) {
                return rc;
            }
            i = j + 1;
        } else {
            int is_last = (i + 1 == nal_count) ? 1 : 0;
            int rc = h265_emit_single(n->data, n->len, is_last, cb, userdata);
            if (rc != 0) {
                return rc;
            }
            i++;
        }
    }

    return NANORTC_OK;
}

int h265_packetize(const uint8_t *nalu, size_t nalu_len, size_t mtu, h265_packet_cb cb,
                   void *userdata)
{
    if (!nalu || nalu_len < H265_NAL_HEADER_SIZE || mtu < H265_FU_HEADER_SIZE + 1 || !cb) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    /* Fast path: Single NAL Unit Packet (§4.4.1). */
    if (nalu_len <= mtu) {
        return h265_emit_single(nalu, nalu_len, 1 /* marker: last packet of one-NAL AU */, cb,
                                userdata);
    }
    /* Slow path: fragment into an FU sequence (§4.4.3). */
    return h265_emit_fu(nalu, nalu_len, mtu, 1 /* marker_last_au */, cb, userdata);
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
            NANORTC_LOGW("H265", "FU interrupted by AP");
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
                NANORTC_LOGW("H265", "AP sub-NAL exceeds packet");
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
                NANORTC_LOGW("H265", "FU start exceeds buffer");
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
                NANORTC_LOGW("H265", "FU reassembly exceeds buffer");
                d->in_progress = 0;
                d->len = 0;
                return NANORTC_ERR_BUFFER_TOO_SMALL;
            }
            memcpy(d->buf + d->len, frag_data, frag_len);
            d->len += frag_len;
        } else {
            /* FU continuation without a prior Start fragment — drop. */
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

    /* --------------------------------------------------------------
     * PACI (RFC 7798 §4.4.4) — type 50. Out of scope for first pass.
     * Silently ignore, matching nano_h264.c's handling of unknown
     * NAL types.
     * -------------------------------------------------------------- */
    if (type == H265_PKT_PACI) {
        NANORTC_LOGD("H265", "PACI packet ignored");
        return NANORTC_OK;
    }

    /* Types 51..63 are reserved per RFC 7798 §4.4. Ignore. */
    NANORTC_LOGD("H265", "unknown H.265 RTP payload type");
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
