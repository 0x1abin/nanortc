/*
 * nanortc — Adaptive media rate controller (VIDEO feature, opt-in)
 *
 * Pure-compute spec selection for Phase 14. Given the bottleneck estimate, the
 * smoothed loss fraction and a caller-supplied capability ladder, recommend the
 * spec rung {resolution, fps, bitrate} the path can carry, ranking latency +
 * smoothness above picture detail.
 *
 * Asymmetric hysteresis: down-steps apply immediately (back off fast on
 * congestion, possibly skipping rungs); up-steps are one rung at a time and
 * only after the estimate clears the next rung's bitrate by a headroom margin
 * AND that condition has held for a minimum time — so the recommendation never
 * flaps between rungs on a noisy estimate.
 *
 * Sans-I/O: this file touches no encoder, socket, or clock. The library
 * decides; the caller applies the rung to its encoder.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h" /* umbrella: defines nanortc_spec_rung_t, pulls in nano_rate_control.h */
#include "nano_rate_control.h"
#include "nano_log.h"
#include <string.h>

#if NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_RATE_CONTROL

/* ================================================================
 * Init
 * ================================================================ */

int rate_control_init(nano_rate_control_t *rc)
{
    if (!rc) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    memset(rc, 0, sizeof(*rc));
    return NANORTC_OK;
}

/* ================================================================
 * Internal helpers
 * ================================================================ */

/* Disarm the up-step hold timer. up_target == 0 means "not arming": a valid
 * up-target is always cur_rung+1 >= 1, so 0 is an unambiguous sentinel and the
 * timer logic never depends on now_ms == 0 being special. */
static void rate_control_clear_up(nano_rate_control_t *rc)
{
    rc->up_target = 0;
    rc->up_since_ms = 0;
}

/* Highest rung whose bitrate fits inside the safety-discounted budget. The
 * ladder is ordered ascending by bitrate, so the last satisfying index wins.
 * Rung 0 is the floor: if even the lowest rung exceeds the budget we still
 * recommend it (there is nothing lower to fall back to). */
static uint8_t rate_control_select_target(uint32_t estimate_bps, const nanortc_spec_rung_t *ladder,
                                          uint8_t n)
{
    uint64_t budget = (uint64_t)estimate_bps * (uint64_t)NANORTC_RATE_CONTROL_SAFETY_PCT / 100u;
    uint8_t target = 0;
    for (uint8_t i = 0; i < n; i++) {
        if ((uint64_t)ladder[i].bitrate_bps <= budget) {
            target = i;
        }
    }
    return target;
}

/* ================================================================
 * Update
 * ================================================================ */

int rate_control_update(nano_rate_control_t *rc, uint32_t estimate_bps, uint16_t loss_q8,
                        const nanortc_spec_rung_t *ladder, uint8_t n, uint32_t now_ms)
{
    if (!rc || !ladder || n == 0) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    /* No feedback yet: hold the current rung, or a provisional floor before the
     * first real selection. Do NOT latch has_rung — wait for a real estimate. */
    if (estimate_bps == 0) {
        return rc->has_rung ? rc->cur_rung : 0;
    }

    uint8_t target = rate_control_select_target(estimate_bps, ladder, n);

    /* First real selection latches directly to its target — no up-gating on a
     * cold start, so the stream reaches a usable rung immediately. */
    if (!rc->has_rung) {
        rc->cur_rung = target;
        rc->has_rung = 1;
        rate_control_clear_up(rc);
        return rc->cur_rung;
    }

    /* Smoothness-first: sustained loss forces at least one rung down even when
     * the rate estimate alone would hold the current rung. */
    if (loss_q8 >= NANORTC_RATE_CONTROL_LOSS_DOWN_Q8 && rc->cur_rung > 0) {
        uint8_t loss_target = (uint8_t)(rc->cur_rung - 1);
        if (loss_target < target) {
            target = loss_target;
        }
    }

    if (target < rc->cur_rung) {
        /* Down: back off fast, possibly several rungs at once. */
        rc->cur_rung = target;
        rate_control_clear_up(rc);
        NANORTC_LOGD("RCTL", "spec step down");
    } else if (target > rc->cur_rung) {
        /* Up: one rung at a time, gated by headroom AND a minimum hold time. */
        uint8_t next = (uint8_t)(rc->cur_rung + 1);
        uint64_t need = (uint64_t)ladder[next].bitrate_bps *
                        (100u + (uint64_t)NANORTC_RATE_CONTROL_UP_HEADROOM_PCT) / 100u;
        if ((uint64_t)estimate_bps >= need) {
            if (rc->up_target != next) {
                /* (Re)arm the hold timer for this up-step. */
                rc->up_target = next;
                rc->up_since_ms = now_ms;
            }
            if ((uint32_t)(now_ms - rc->up_since_ms) >= NANORTC_RATE_CONTROL_MIN_HOLD_MS) {
                rc->cur_rung = next;
                rate_control_clear_up(rc);
                NANORTC_LOGD("RCTL", "spec step up");
            }
        } else {
            /* Headroom lapsed before the hold elapsed — disarm. */
            rate_control_clear_up(rc);
        }
    } else {
        /* Target equals the current rung: stable, disarm any pending up-step. */
        rate_control_clear_up(rc);
    }

    return rc->cur_rung;
}

/* ================================================================
 * Getter
 * ================================================================ */

uint8_t rate_control_get_rung(const nano_rate_control_t *rc)
{
    if (!rc) {
        return 0;
    }
    return rc->cur_rung;
}

#endif /* NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_RATE_CONTROL */
