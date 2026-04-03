/*
 * nanortc — Bandwidth estimation (MEDIA profile only)
 *
 * Implements REMB (Receiver Estimated Maximum Bitrate) parsing and
 * exponential moving average (EMA) smoothing for bitrate adaptation.
 *
 * REMB format (draft-alvestrand-rmcat-remb-03 §2):
 *
 *   0                   1                   2                   3
 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |V=2|P| FMT=15  |   PT=206      |             length            |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                  SSRC of packet sender                        |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                  SSRC of media source (unused, 0)             |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |  Unique identifier 'R' 'E' 'M' 'B'                           |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |  Num SSRC     | BR Exp    |  BR Mantissa                      |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |   SSRC feedback (4 bytes each, repeated Num SSRC times)       |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_bwe.h"
#include "nano_log.h"
#include "nanortc.h"
#include <string.h>

/* ================================================================
 * Init
 * ================================================================ */

int bwe_init(nano_bwe_t *bwe)
{
    if (!bwe) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    memset(bwe, 0, sizeof(*bwe));
    bwe->estimated_bitrate = NANORTC_BWE_INITIAL_BITRATE;
    return NANORTC_OK;
}

/* ================================================================
 * REMB parser (draft-alvestrand-rmcat-remb-03 §2)
 * ================================================================ */

int bwe_parse_remb(const uint8_t *data, size_t len, uint32_t *bitrate)
{
    if (!data || !bitrate) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    /* Minimum REMB size: RTCP header (4) + sender SSRC (4) +
     * media SSRC (4) + "REMB" (4) + BR fields (4) = 20 bytes */
    if (len < 20) {
        return NANORTC_ERR_PARSE;
    }

    /* Validate RTCP version = 2 */
    uint8_t version = (data[0] >> 6) & 0x03;
    if (version != 2) {
        return NANORTC_ERR_PARSE;
    }

    /* Check FMT=15 and PT=206 (PSFB) */
    uint8_t fmt = data[0] & 0x1F;
    uint8_t pt = data[1];
    if (fmt != BWE_REMB_FMT || pt != 206) {
        return NANORTC_ERR_PARSE;
    }

    /* Validate packet length */
    uint16_t length_words = nanortc_read_u16be(data + 2);
    uint32_t pkt_size = ((uint32_t)length_words + 1) * 4;
    if (pkt_size > len || pkt_size < 20) {
        return NANORTC_ERR_PARSE;
    }

    /* Check "REMB" unique identifier at offset 12 */
    uint32_t uid = nanortc_read_u32be(data + 12);
    if (uid != BWE_REMB_UNIQUE_ID) {
        return NANORTC_ERR_PARSE;
    }

    /* Parse BR Exp and BR Mantissa at offset 16:
     * byte 16: Num SSRC (8 bits)
     * byte 17: BR Exp (6 bits) | BR Mantissa high (2 bits)
     * byte 18-19: BR Mantissa low (16 bits)
     *
     * bitrate = mantissa * 2^exp */
    uint8_t br_exp = (data[17] >> 2) & 0x3F;
    uint32_t br_mantissa =
        ((uint32_t)(data[17] & 0x03) << 16) | ((uint32_t)data[18] << 8) | (uint32_t)data[19];

    /* Compute bitrate, clamping to avoid overflow */
    if (br_exp >= 32 || (br_exp > 0 && br_mantissa > (UINT32_MAX >> br_exp))) {
        *bitrate = UINT32_MAX; /* Saturate */
    } else {
        *bitrate = br_mantissa << br_exp;
    }

    return NANORTC_OK;
}

/* ================================================================
 * RTCP feedback handler
 * ================================================================ */

int bwe_on_rtcp_feedback(nano_bwe_t *bwe, const uint8_t *data, size_t len, uint32_t now_ms)
{
    if (!bwe || !data) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    uint32_t remb_bitrate = 0;
    int rc = bwe_parse_remb(data, len, &remb_bitrate);
    if (rc != NANORTC_OK) {
        return rc; /* Not a REMB packet or parse error */
    }

    /* Clamp to configured min/max */
    if (remb_bitrate < NANORTC_BWE_MIN_BITRATE) {
        remb_bitrate = NANORTC_BWE_MIN_BITRATE;
    } else if (remb_bitrate > NANORTC_BWE_MAX_BITRATE) {
        remb_bitrate = NANORTC_BWE_MAX_BITRATE;
    }

    bwe->last_remb_bitrate = remb_bitrate;
    bwe->remb_count++;

    /* Exponential moving average (EMA) smoothing:
     * new = (alpha * remb + (256 - alpha) * old) / 256
     *
     * On first REMB, jump directly to the received value. */
    if (bwe->remb_count == 1) {
        bwe->estimated_bitrate = remb_bitrate;
    } else {
        uint32_t alpha = NANORTC_BWE_EMA_ALPHA;
        uint64_t smoothed =
            ((uint64_t)alpha * remb_bitrate + (uint64_t)(256 - alpha) * bwe->estimated_bitrate) /
            256;
        bwe->estimated_bitrate = (uint32_t)smoothed;
    }

    bwe->last_update_ms = now_ms;

    NANORTC_LOGD("BWE", "REMB received, estimate updated");

    return NANORTC_OK;
}

/* ================================================================
 * Getter
 * ================================================================ */

uint32_t bwe_get_bitrate(const nano_bwe_t *bwe)
{
    if (!bwe) {
        return 0;
    }
    return bwe->estimated_bitrate;
}
