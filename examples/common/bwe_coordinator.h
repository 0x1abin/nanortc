/*
 * bwe_coordinator — shared BWE → encoder apply glue for example publishers
 *
 * Examples that multiplex one hardware encoder across many viewers (macOS
 * VideoToolbox camera, RK3588 MPP camera, future ones) all need the same
 * three-step loop:
 *
 *   1. walk the per-viewer BWE estimates, pick the minimum (slowest link wins)
 *   2. skip if we've applied a target in the last ~1 s (rate-limit)
 *   3. skip if the new candidate is within ±5 % of the currently applied value
 *      (the nanortc event threshold is already ~15 %, but multiple viewers
 *      crossing their own thresholds can average out into micro-moves here)
 *
 * This module owns step 2 + 3 + the encoder hook. Step 1 stays app-side
 * because the session/viewer storage differs per example (session_t[] vs
 * viewer-id keyed slot table).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_EXAMPLES_COMMON_BWE_COORDINATOR_H_
#define NANORTC_EXAMPLES_COMMON_BWE_COORDINATOR_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Encoder bitrate setter signature.
 *
 * @param new_bps  Target bitrate in bits/s, already clamped to the
 *                 coordinator-owned bounds (caller is responsible for
 *                 feeding min/max-clamped values).
 * @param ctx      Opaque pointer passed through from bwe_coordinator_init().
 * @return 0 on success, non-zero to abort the apply (coordinator state
 *         will not be updated so the next call will retry).
 */
typedef int (*bwe_apply_fn)(uint32_t new_bps, void *ctx);

typedef struct bwe_coordinator {
    /* Config — set once via bwe_coordinator_init(). */
    uint32_t apply_interval_ms; /* throttle floor, typical 1000 */
    uint8_t dampen_pct;         /* dead-band around applied_bps, typical 5 */
    bwe_apply_fn apply;
    void *apply_ctx;

    /* State — opaque to callers. */
    uint32_t applied_bps;
    uint32_t last_apply_ms;
} bwe_coordinator_t;

/**
 * @brief Initialize a coordinator in-place.
 */
void bwe_coordinator_init(bwe_coordinator_t *c, uint32_t apply_interval_ms,
                          uint8_t dampen_pct, bwe_apply_fn apply, void *ctx);

/**
 * @brief Possibly push a new candidate target to the encoder.
 *
 * Call this after computing @p candidate_bps as the min across the app's
 * per-viewer estimate storage.
 *
 * @return 0 applied, >0 intentionally skipped (see BWE_APPLY_* codes),
 *         <0 apply callback reported failure.
 */
int bwe_coordinator_try_apply(bwe_coordinator_t *c, uint32_t candidate_bps,
                              int contributors, uint32_t now_ms);

/* Skip reasons (positive return codes from try_apply). */
#define BWE_APPLY_OK                0  /* encoder was updated */
#define BWE_APPLY_SKIP_INTERVAL     1  /* last apply was too recent */
#define BWE_APPLY_SKIP_DAMPEN       2  /* candidate inside ±dampen_pct of applied */
#define BWE_APPLY_SKIP_NO_CONTRIB   3  /* no live contributor (candidate==0) */

/**
 * @brief Reset the coordinator's applied/last-apply state.
 *
 * Useful when the publisher tears down and rebuilds the session pool
 * (e.g. MQTT disconnect + reconnect) so the next candidate isn't rejected
 * by the dampen band against a stale `applied_bps` from a dead session.
 */
void bwe_coordinator_reset(bwe_coordinator_t *c);

/* Enum → string for nanortc_ev_bitrate_estimate_t.{direction,source}. Used
 * by every example's log line; centralised here so the formatter stays
 * consistent. Returns a static string; never NULL. */
const char *bwe_dir_str(uint8_t dir);
const char *bwe_src_str(uint8_t src);

#ifdef __cplusplus
}
#endif

#endif /* NANORTC_EXAMPLES_COMMON_BWE_COORDINATOR_H_ */
