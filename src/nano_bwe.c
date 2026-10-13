/*
 * nanortc — Bandwidth estimation (MEDIA profile only)
 *
 * Implements REMB (Receiver Estimated Maximum Bitrate) parsing and
 * bounded loss control and matched transport-feedback probing.
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
#include "nanortc.h"
#include "nano_time.h"
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
    bwe->local_target = NANORTC_BWE_INITIAL_BITRATE;
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

int bwe_set_bounds(nano_bwe_t *bwe, uint32_t min_bps, uint32_t max_bps)
{
    if (!bwe) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    uint32_t effective_min = min_bps ? min_bps : (uint32_t)NANORTC_BWE_MIN_BITRATE;
    uint32_t effective_max = max_bps ? max_bps : (uint32_t)NANORTC_BWE_MAX_BITRATE;
    if (effective_min > effective_max) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    /* Commit only after validating the fully-expanded envelope. */
    bwe->runtime_min_bps = min_bps;
    bwe->runtime_max_bps = max_bps;
    bwe->local_target = bwe_clamp(bwe, bwe->local_target);
    bwe->estimated_bitrate = bwe->local_target;
    if (bwe->remb_count && bwe->estimated_bitrate > bwe->remb_limit)
        bwe->estimated_bitrate = bwe->remb_limit;
    bwe->prev_event_bitrate = bwe_clamp(bwe, bwe->prev_event_bitrate);
    return NANORTC_OK;
}

int bwe_set_initial_bitrate(nano_bwe_t *bwe, uint32_t bps)
{
    if (!bwe) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    if (bwe->remb_count == 0 && bwe->twcc_count == 0) {
        uint32_t initial = bps ? bps : (uint32_t)NANORTC_BWE_INITIAL_BITRATE;
        bwe->local_target = bwe_clamp(bwe, initial);
        bwe->estimated_bitrate = bwe->local_target;
        bwe->prev_event_bitrate = bwe->estimated_bitrate;
    }
    return NANORTC_OK;
}

#define BWE_SENT_VALID     1u
#define BWE_SENT_REPORTED  2u
#define BWE_SENT_AMBIGUOUS 4u

static void bwe_publish(nano_bwe_t *bwe, uint32_t now_ms)
{
    bwe->estimated_bitrate = bwe_clamp(bwe, bwe->local_target);
    /* A receiver cap overrides encoder preference, including its minimum. */
    if (bwe->remb_count && bwe->estimated_bitrate > bwe->remb_limit)
        bwe->estimated_bitrate = bwe->remb_limit;
    bwe->last_update_ms = now_ms;
}

int bwe_on_rtcp_feedback(nano_bwe_t *bwe, const uint8_t *data, size_t len, uint32_t now_ms)
{
    if (!bwe || !data)
        return NANORTC_ERR_INVALID_PARAM;
    uint32_t cap = 0;
    int rc = bwe_parse_remb(data, len, &cap);
    if (rc != NANORTC_OK)
        return rc;
    bwe->remb_limit = cap;
    bwe->remb_count++;
    if (bwe->twcc_count == 0)
        bwe->local_target = bwe_clamp(bwe, cap);
    else if (bwe->local_target > cap)
        bwe->local_target = cap;
    bwe_publish(bwe, now_ms);
    return NANORTC_OK;
}

static void bwe_roll_send_window(nano_bwe_t *bwe, uint32_t now_ms)
{
    if (!bwe->bucket_valid) {
        bwe->bucket_start_ms = now_ms;
        bwe->bucket_valid = true;
        return;
    }
    uint32_t elapsed = nano_time_elapsed(now_ms, bwe->bucket_start_ms);
    if (elapsed > INT32_MAX)
        return; /* A delayed completion must not roll the clock backward. */
    uint32_t steps = elapsed / NANORTC_BWE_CONTROL_INTERVAL_MS;
    if (steps == 0)
        return;
    if (steps >= NANORTC_BWE_RATE_BUCKETS)
        memset(bwe->send_bytes, 0, sizeof(bwe->send_bytes));
    else {
        for (uint32_t i = NANORTC_BWE_RATE_BUCKETS; i-- > steps;)
            bwe->send_bytes[i] = bwe->send_bytes[i - steps];
        for (uint32_t i = 0; i < steps; i++)
            bwe->send_bytes[i] = 0;
    }
    bwe->bucket_start_ms += steps * NANORTC_BWE_CONTROL_INTERVAL_MS;
}

void bwe_record_drop(nano_bwe_t *bwe, uint32_t at_ms)
{
    if (!bwe)
        return;
    bwe->last_bad_send_ms = at_ms;
    bwe->bad_send_valid = true;
    bwe->stable_valid = false;
}

void bwe_record_transmit(nano_bwe_t *bwe, uint32_t sequence, uint16_t bytes, uint32_t at_ms,
                         bool has_twcc, bool retransmit)
{
    if (!bwe)
        return;
    bwe_roll_send_window(bwe, at_ms);
    bwe->send_bytes[0] += bytes;
    if (retransmit) {
        bwe_record_drop(bwe, at_ms);
        nano_bwe_sent_t *old = &bwe->sent[sequence & (NANORTC_BWE_SEND_HISTORY_SIZE - 1u)];
        if ((old->flags & BWE_SENT_VALID) != 0 && old->sequence == sequence)
            old->flags |= BWE_SENT_AMBIGUOUS;
        return;
    }
    if (!has_twcc)
        return;
    uint32_t advance = sequence - bwe->latest_sequence;
    if (!bwe->sequence_valid || (advance != 0u && advance <= INT32_MAX)) {
        bwe->latest_sequence = sequence;
        bwe->sequence_valid = true;
    }
    nano_bwe_sent_t *record = &bwe->sent[sequence & (NANORTC_BWE_SEND_HISTORY_SIZE - 1u)];
    record->sequence = sequence;
    record->sent_ms = at_ms;
    record->bytes = bytes;
    record->flags = BWE_SENT_VALID;
}

static void bwe_control(nano_bwe_t *bwe, uint16_t loss, uint32_t now_ms, bool allow_growth)
{
    bwe->smoothed_loss_q8 = bwe->twcc_count == 0
                                ? loss
                                : (uint16_t)(((uint32_t)bwe->smoothed_loss_q8 * 3u + loss) / 4u);
    bwe->twcc_count++;
    bwe->last_feedback_ms = now_ms;
    bwe->feedback_valid = true;
    bool clean =
        loss < NANORTC_BWE_LOW_LOSS_Q8 && allow_growth && bwe->delay_valid && !bwe->delay_growing;
    if (bwe->bad_send_valid &&
        nano_time_elapsed(now_ms, bwe->last_bad_send_ms) < NANORTC_BWE_PROBE_INTERVAL_MS)
        clean = false;
    bwe_roll_send_window(bwe, now_ms);
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < NANORTC_BWE_RATE_BUCKETS; i++)
        bytes += bwe->send_bytes[i];
    uint32_t covered_ms = (NANORTC_BWE_RATE_BUCKETS - 1u) * NANORTC_BWE_CONTROL_INTERVAL_MS +
                          nano_time_elapsed(now_ms, bwe->bucket_start_ms);
    uint64_t load_bps = bytes * 8000u / covered_ms;
    if (load_bps * 100u < (uint64_t)bwe_get_bitrate(bwe) * NANORTC_BWE_MIN_LOAD_PCT)
        clean = false;
    if (!clean)
        bwe->stable_valid = false;
    else if (!bwe->stable_valid) {
        bwe->stable_valid = true;
        bwe->stable_since_ms = now_ms;
    }
    if (bwe->control_valid &&
        nano_time_elapsed(now_ms, bwe->control_ms) < NANORTC_BWE_CONTROL_INTERVAL_MS)
        return;
    bwe->control_ms = now_ms;
    bwe->control_valid = true;
    if (loss > NANORTC_BWE_HIGH_LOSS_Q8) {
        uint64_t shrunk = (uint64_t)bwe->local_target * (512u - loss) / 512u;
        uint32_t min_bps = bwe_effective_min(bwe);
        bwe->local_target = shrunk < min_bps ? min_bps : (uint32_t)shrunk;
    } else if (clean && bwe->stable_valid &&
               nano_time_elapsed(now_ms, bwe->stable_since_ms) >= NANORTC_BWE_STABLE_INTERVAL_MS &&
               nano_time_elapsed(now_ms, bwe->last_probe_ms) >= NANORTC_BWE_PROBE_INTERVAL_MS) {
        uint64_t grown =
            (uint64_t)bwe->local_target * (100u + NANORTC_BWE_PROBE_INCREASE_PCT) / 100u;
        uint32_t max_bps = bwe_effective_max(bwe);
        bwe->local_target = grown > max_bps ? max_bps : (uint32_t)grown;
        bwe->last_probe_ms = now_ms;
    }
    bwe_publish(bwe, now_ms);
}

int bwe_on_twcc_loss(nano_bwe_t *bwe, uint16_t loss_fraction_q8, uint32_t now_ms)
{
    if (!bwe)
        return NANORTC_ERR_INVALID_PARAM;
    if (loss_fraction_q8 > 256u)
        loss_fraction_q8 = 256u;
    /* Summary-only samples may reduce load, but never prove spare capacity. */
    bwe_control(bwe, loss_fraction_q8, now_ms, false);
    return NANORTC_OK;
}

typedef struct {
    nano_bwe_t *bwe;
    uint32_t now_ms;
    uint32_t matched;
    uint32_t unknown;
    uint32_t lost;
    int64_t arrival_us;
} bwe_feedback_t;

static void bwe_feedback_packet(uint16_t wire_seq, nano_twcc_status_t status, int32_t delta_us,
                                void *user)
{
    bwe_feedback_t *feedback = (bwe_feedback_t *)user;
    nano_bwe_t *bwe = feedback->bwe;
    if (status != NANO_TWCC_STATUS_NOT_RECEIVED)
        feedback->arrival_us += delta_us;
    uint32_t seq = bwe->latest_sequence - (uint16_t)((uint16_t)bwe->latest_sequence - wire_seq);
    nano_bwe_sent_t *record = &bwe->sent[seq & (NANORTC_BWE_SEND_HISTORY_SIZE - 1u)];
    if ((record->flags & BWE_SENT_VALID) == 0 || record->sequence != seq ||
        nano_time_elapsed(feedback->now_ms, record->sent_ms) > NANORTC_BWE_HISTORY_MAX_AGE_MS) {
        feedback->unknown++;
        return;
    }
    if ((record->flags & BWE_SENT_REPORTED) != 0)
        return;
    record->flags |= BWE_SENT_REPORTED;
    feedback->matched++;
    if (status == NANO_TWCC_STATUS_NOT_RECEIVED) {
        feedback->lost++;
        return;
    }
    if ((record->flags & BWE_SENT_AMBIGUOUS) != 0 || status == NANO_TWCC_STATUS_RESERVED)
        return;
    /* Late feedback can acknowledge an older send after a newer one. Such
     * samples still count for loss, but must not rewind the delay window. */
    if (bwe->delay_samples != 0 &&
        nano_time_elapsed(record->sent_ms, bwe->delay_first_send_ms) > INT32_MAX)
        return;
    if (bwe->delay_samples == 0) {
        bwe->delay_first_send_ms = record->sent_ms;
        bwe->delay_first_arrival_us = feedback->arrival_us;
    }
    bwe->delay_samples++;
    bwe->delay_last_arrival_us = feedback->arrival_us;
    uint32_t span = nano_time_elapsed(record->sent_ms, bwe->delay_first_send_ms);
    if (span < NANORTC_BWE_DELAY_MIN_SPAN_MS || bwe->delay_samples < NANORTC_BWE_DELAY_MIN_SAMPLES)
        return;
    int64_t excess = feedback->arrival_us - bwe->delay_first_arrival_us - (int64_t)span * 1000;
    bwe->delay_valid = true;
    bwe->delay_growing = excess > NANORTC_BWE_DELAY_GROWTH_US;
    if (bwe->delay_growing)
        bwe->stable_valid = false;
    if (span >= NANORTC_BWE_CONTROL_INTERVAL_MS) {
        bwe->delay_samples = 1;
        bwe->delay_first_send_ms = record->sent_ms;
        bwe->delay_first_arrival_us = feedback->arrival_us;
    }
}

int bwe_on_twcc_feedback(nano_bwe_t *bwe, const uint8_t *data, size_t len, uint32_t now_ms)
{
    if (!bwe || !data)
        return NANORTC_ERR_INVALID_PARAM;
    nano_twcc_summary_t summary;
    int rc = twcc_parse_feedback(data, len, &summary, NULL, NULL);
    if (rc != NANORTC_OK)
        return rc;
    if (!bwe->feedback_valid ||
        nano_time_elapsed(now_ms, bwe->last_feedback_ms) > NANORTC_BWE_FEEDBACK_TIMEOUT_MS) {
        bwe->delay_samples = 0;
        bwe->delay_valid = false;
        bwe->stable_valid = false;
    }
    int64_t arrival = summary.reference_time_us;
    if (bwe->delay_samples != 0) {
        const int64_t period = INT64_C(16777216) * 64000;
        int64_t delta = (arrival - bwe->delay_last_arrival_us) % period;
        if (delta < -period / 2)
            delta += period;
        else if (delta > period / 2)
            delta -= period;
        arrival = bwe->delay_last_arrival_us + delta;
    }
    bwe_feedback_t feedback = {bwe, now_ms, 0, 0, 0, arrival};
    rc = twcc_parse_feedback(data, len, &summary, bwe_feedback_packet, &feedback);
    if (rc != NANORTC_OK)
        return rc;
    bwe->history_missed += feedback.unknown;
    if (feedback.matched == 0) {
        if (feedback.unknown != 0)
            bwe->stable_valid = false;
        return NANORTC_OK;
    }
    uint16_t loss = (uint16_t)((feedback.lost * 256u) / feedback.matched);
    bool enough = feedback.matched * 100u >=
                  (feedback.matched + feedback.unknown) * NANORTC_BWE_MIN_HISTORY_MATCH_PCT;
    bwe_control(bwe, loss, now_ms, enough);
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

uint16_t bwe_get_loss_q8(const nano_bwe_t *bwe)
{
    if (!bwe) {
        return 0;
    }
    return bwe->smoothed_loss_q8;
}

/* ================================================================
 * Event threshold check
 * ================================================================ */

int bwe_should_emit_event(nano_bwe_t *bwe)
{
    if (!bwe)
        return 0;
    if (bwe->prev_event_bitrate == 0) {
        if (bwe->estimated_bitrate == 0)
            return 0;
        bwe->prev_event_bitrate = bwe->estimated_bitrate;
        return 1;
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
