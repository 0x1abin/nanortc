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
#include <stdbool.h>
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
    bwe->prev_event_bitrate = NANORTC_BWE_INITIAL_BITRATE;
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

/* REMB source ID = 0 (NANORTC_BWE_SRC_REMB),
 * TWCC loss source ID = 1 (NANORTC_BWE_SRC_TWCC_LOSS).
 * We keep the numeric values here rather than including the public
 * header so src/ stays free of public-API inclusion cycles. */
#define BWE_SRC_REMB      0
#define BWE_SRC_TWCC_LOSS 1

static uint32_t bwe_effective_min(const nano_bwe_t *bwe)
{
    return bwe->runtime_min_bps ? bwe->runtime_min_bps : (uint32_t)NANORTC_BWE_MIN_BITRATE;
}

static uint32_t bwe_effective_max(const nano_bwe_t *bwe)
{
    return bwe->runtime_max_bps ? bwe->runtime_max_bps : (uint32_t)NANORTC_BWE_MAX_BITRATE;
}

static uint32_t bwe_clamp(const nano_bwe_t *bwe, uint32_t bps)
{
    uint32_t min_bps = bwe_effective_min(bwe);
    uint32_t max_bps = bwe_effective_max(bwe);
    if (bps < min_bps) {
        return min_bps;
    }
    if (bps > max_bps) {
        return max_bps;
    }
    return bps;
}

static void bwe_apply(nano_bwe_t *bwe, uint32_t candidate_bps, uint8_t source, uint32_t now_ms,
                      bool first_sample)
{
    candidate_bps = bwe_clamp(bwe, candidate_bps);

    if (first_sample) {
        bwe->estimated_bitrate = candidate_bps;
    } else {
        uint32_t alpha = NANORTC_BWE_EMA_ALPHA;
        uint64_t smoothed =
            ((uint64_t)alpha * candidate_bps + (uint64_t)(256 - alpha) * bwe->estimated_bitrate) /
            256;
        bwe->estimated_bitrate = (uint32_t)smoothed;
    }
    bwe->last_update_ms = now_ms;
    bwe->last_source = source;
}

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

    bwe->remb_count++;
    bwe->last_remb_bitrate = bwe_clamp(bwe, remb_bitrate);

    bwe_apply(bwe, remb_bitrate, BWE_SRC_REMB, now_ms, bwe->remb_count == 1);

    NANORTC_LOGD("BWE", "REMB received, estimate updated");

    return NANORTC_OK;
}

int bwe_on_twcc_loss(nano_bwe_t *bwe, uint16_t loss_fraction_q8, uint32_t now_ms)
{
    if (!bwe) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (loss_fraction_q8 > 256) {
        loss_fraction_q8 = 256; /* saturate */
    }

    uint32_t max_bps = bwe_effective_max(bwe);
    uint32_t min_bps = bwe_effective_min(bwe);

    /* Loss-based controller, legacy libwebrtc flavour. Thresholds measured
     * in 256-ths of a packet so we stay in integer math. */
    uint32_t target = bwe->estimated_bitrate;
    if (target == 0) {
        target = NANORTC_BWE_INITIAL_BITRATE;
    }

    if (loss_fraction_q8 < 5) {
        /* Low loss: grow aggressively, +8 % per feedback. Cap at max. */
        uint64_t grown = (uint64_t)target * 108u / 100u;
        target = (grown > max_bps) ? max_bps : (uint32_t)grown;
    } else if (loss_fraction_q8 > 25) {
        /* High loss: new = old * (1 - 0.5 * loss_fraction).
         * loss_fraction = loss_q8 / 256, so new = old * (512 - loss_q8) / 512. */
        uint64_t shrunk = (uint64_t)target * (uint64_t)(512 - loss_fraction_q8) / 512u;
        if (shrunk < min_bps) {
            shrunk = min_bps;
        }
        target = (uint32_t)shrunk;
    }
    /* else: hold */

    bwe->twcc_count++;
    /* First TWCC sample jumps directly to target (like REMB) so the first
     * feedback is not blended with the compile-time initial estimate. */
    bwe_apply(bwe, target, BWE_SRC_TWCC_LOSS, now_ms, bwe->twcc_count == 1);

    NANORTC_LOGD("BWE", "TWCC loss sample processed");
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

/* ================================================================
 * Event threshold check
 * ================================================================ */

int bwe_should_emit_event(nano_bwe_t *bwe)
{
    if (!bwe || bwe->prev_event_bitrate == 0) {
        return 0;
    }

    /* Calculate absolute percentage change:
     * |current - prev| * 100 / prev > threshold */
    uint32_t cur = bwe->estimated_bitrate;
    uint32_t prev = bwe->prev_event_bitrate;
    uint32_t diff = (cur > prev) ? (cur - prev) : (prev - cur);

    uint8_t threshold_pct = bwe->runtime_event_threshold_pct ? bwe->runtime_event_threshold_pct
                                                             : NANORTC_BWE_EVENT_THRESHOLD_PCT;

    /* Use integer math: diff * 100 / prev > threshold
     * Rearranged to avoid overflow: diff > prev * threshold / 100 */
    uint32_t threshold_abs = prev / 100 * threshold_pct;

    if (diff > threshold_abs) {
        bwe->prev_event_bitrate = cur;
        return 1;
    }
    return 0;
}
