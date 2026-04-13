/*
 * nanortc examples — Epoch-based media frame pacer
 *
 * Drift-free pacing for fixed-interval media sources (20ms audio,
 * 40ms video, etc.). Computes the number of frames that should have
 * been sent by `now_ms` since the first tick and lets the caller
 * loop sending until caught up, with burst-skip when falling behind.
 *
 * Usage:
 *   nano_media_pacer_t pacer = { .interval_ms = 20 };  // 20ms Opus
 *   while (connected) {
 *       uint32_t due = nano_media_pacer_due(&pacer, now_ms);
 *       for (uint32_t i = 0; i < due; i++) {
 *           if (read_and_send_one_frame() < 0) break;
 *           nano_media_pacer_advance(&pacer);
 *       }
 *   }
 *
 *   // On (re)connect, reset so the epoch re-anchors to the current time:
 *   nano_media_pacer_reset(&pacer);
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_MEDIA_PACER_H_
#define NANORTC_MEDIA_PACER_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t interval_ms; /* frame interval (e.g. 20 for Opus, 40 for 25fps) */
    uint32_t epoch_ms;    /* wall-clock anchor (0 = not yet set) */
    uint32_t frame_count; /* frames advanced since epoch */
} nano_media_pacer_t;

/*
 * Return the number of frames whose scheduled send time has passed but
 * have not yet been marked sent (via nano_media_pacer_advance).
 *
 * On first call, anchors the epoch to `now_ms` (returns 0 the first time).
 * If the caller has fallen more than 2 frames behind, skips ahead so only
 * one frame is due (avoids bursty catch-up on jitter/GC stalls).
 */
static inline uint32_t nano_media_pacer_due(nano_media_pacer_t *p, uint32_t now_ms)
{
    if (p->epoch_ms == 0) {
        p->epoch_ms = now_ms;
    }
    if (p->interval_ms == 0) {
        return 0;
    }
    uint32_t target = (now_ms - p->epoch_ms) / p->interval_ms;
    /* Skip burst: if behind by >2 frames, fast-forward so only 1 is due. */
    if (target > p->frame_count + 2) {
        p->frame_count = target - 1;
    }
    return (target > p->frame_count) ? (target - p->frame_count) : 0;
}

/* Mark one frame as sent. */
static inline void nano_media_pacer_advance(nano_media_pacer_t *p)
{
    p->frame_count++;
}

/* Re-anchor the epoch on the next tick (call on (re)connect). */
static inline void nano_media_pacer_reset(nano_media_pacer_t *p)
{
    p->epoch_ms = 0;
    p->frame_count = 0;
}

#ifdef __cplusplus
}
#endif

#endif /* NANORTC_MEDIA_PACER_H_ */
