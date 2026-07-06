/*
 * nanortc — Adaptive media rate controller internal interface (VIDEO feature)
 *
 * Pure-compute controller for Phase 14 (adaptive video spec selection). Maps a
 * congestion signal (BWE estimate + smoothed loss) plus a caller-supplied
 * capability ladder of {resolution, fps, bitrate} rungs to a recommended rung,
 * applying asymmetric hysteresis (step down fast on congestion, step up only
 * after confirmed, sustained headroom) so the recommendation does not flap.
 *
 * The default policy ranks latency + smoothness above picture detail: it steps
 * resolution/fps down to protect per-pixel quality rather than letting the
 * encoder starve at a fixed geometry (core belief #10). The module is pure —
 * it touches no encoder, socket, or clock; @c now_ms is caller-supplied. The
 * library DECIDES; the caller APPLIES the recommendation to its encoder.
 *
 * @internal Not part of the public API. The recommendation rung type
 *           (nanortc_spec_rung_t) is public (include/nanortc.h); the controller
 *           itself is internal.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_RATE_CONTROL_H_
#define NANORTC_RATE_CONTROL_H_

#include <stdint.h>
#include "nanortc_config.h"
/* nanortc_spec_rung_t comes from the nanortc.h umbrella: it defines the rung
 * type and then includes this header (the same pattern by which nano_media.h
 * uses nanortc_direction_t). Reach this header via nanortc.h, not standalone. */

#if NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_RATE_CONTROL

/**
 * @brief Adaptive media rate controller state.
 *
 * Small and pure: the capability ladder is NOT stored here (it is caller-owned
 * and passed to rate_control_update() each call), so the only persistent state
 * is the current rung plus the up-step hold timer.
 */
typedef struct nano_rate_control {
    uint8_t cur_rung;     /* current recommended rung index into the ladder */
    uint8_t has_rung;     /* 0 until the first real (estimate>0) selection */
    uint8_t up_target;    /* rung we are timing an up-step toward (cur_rung+1) */
    uint32_t up_since_ms; /* monotonic time the up-condition first held */
} nano_rate_control_t;

/**
 * @brief Initialize the rate controller.
 * @param rc  Controller state.
 * @return NANORTC_OK on success, NANORTC_ERR_INVALID_PARAM if @p rc is NULL.
 */
int rate_control_init(nano_rate_control_t *rc);

/**
 * @brief Recompute the recommended spec rung from the congestion signal.
 *
 * Policy (realtime + smoothness first):
 *   - Target rung = highest rung whose @c bitrate_bps fits inside
 *     estimate × NANORTC_RATE_CONTROL_SAFETY_PCT/100 (rung 0 is the floor).
 *   - Loss at/above NANORTC_RATE_CONTROL_LOSS_DOWN_Q8 forces at least one rung
 *     down regardless of the estimate.
 *   - A down-step applies immediately and may skip rungs (back off fast).
 *   - An up-step is one rung at a time and only after the estimate clears the
 *     next rung's bitrate by NANORTC_RATE_CONTROL_UP_HEADROOM_PCT AND that
 *     condition has held for NANORTC_RATE_CONTROL_MIN_HOLD_MS (no flapping).
 *   - The first call with a non-zero estimate latches directly to its target
 *     (no up-gating on cold start).
 *
 * The ladder MUST be ordered ascending by @c bitrate_bps (rung 0 = lowest spec).
 *
 * @param rc            Controller state.
 * @param estimate_bps  Current bottleneck estimate (bps), e.g. bwe_get_bitrate().
 *                      0 = no feedback yet (returns a provisional floor rung).
 * @param loss_q8       Smoothed loss fraction (q8, 256 = 100%), e.g.
 *                      bwe_get_loss_q8().
 * @param ladder        Caller-owned array of capability rungs (not stored).
 * @param n             Number of rungs in @p ladder (1..255).
 * @param now_ms        Current monotonic time in milliseconds.
 * @return The recommended rung index (>= 0) on success, or a negative
 *         NANORTC_ERR_* code.
 * @retval NANORTC_ERR_INVALID_PARAM  @p rc or @p ladder is NULL, or @p n == 0.
 */
int rate_control_update(nano_rate_control_t *rc, uint32_t estimate_bps, uint16_t loss_q8,
                        const nanortc_spec_rung_t *ladder, uint8_t n, uint32_t now_ms);

/**
 * @brief Current recommended rung index.
 * @param rc  Controller state.
 * @return The current rung, or 0 if @p rc is NULL or no selection has been made.
 */
uint8_t rate_control_get_rung(const nano_rate_control_t *rc);

#endif /* NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_RATE_CONTROL */

#endif /* NANORTC_RATE_CONTROL_H_ */
