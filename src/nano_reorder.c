/*
 * nanortc — Video receive reorder buffer
 *
 * See nano_reorder.h. Window indexed by (seq & (SLOTS-1)); each slot validates
 * its stored seq so a stale entry left by a force-advance is treated as empty.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_reorder.h"

#include "nanortc.h" /* NANORTC_OK / NANORTC_ERR_* */

#include <string.h>

#if NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_REORDER

void reorder_init(nano_reorder_t *r)
{
    if (r) {
        memset(r, 0, sizeof(*r));
    }
}

static inline uint16_t reorder_slot_idx(uint16_t seq)
{
    return (uint16_t)(seq & (NANORTC_VIDEO_REORDER_SLOTS - 1));
}

int reorder_push(nano_reorder_t *r, uint16_t seq, uint32_t ts, uint8_t marker, const uint8_t *data,
                 size_t len, uint32_t now_ms)
{
    if (!r || !data || len == 0 || len > NANORTC_MEDIA_BUF_SIZE) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    if (!r->inited) {
        r->inited = true;
        r->next_seq = seq;
    }

    int16_t diff = (int16_t)(seq - r->next_seq);
    if (diff < 0) {
        return NANORTC_OK; /* already released or skipped — late/duplicate, drop */
    }

    if (diff >= (int16_t)NANORTC_VIDEO_REORDER_SLOTS) {
        /* Farther ahead than the window can hold. Advance next_seq so this
         * packet just fits at the window's far edge; the span we jumped over is
         * lost and surfaces via reorder_pop()'s `lost` flag. Stale slots left
         * behind are ignored on pop (their stored seq won't match next_seq). */
        r->next_seq = (uint16_t)(seq - NANORTC_VIDEO_REORDER_SLOTS + 1);
        r->pending_lost = true;
    }

    nano_reorder_slot_t *s = &r->slots[reorder_slot_idx(seq)];
    /* Drop an exact duplicate already buffered; otherwise (incl. a stale slot
     * from a wrapped older seq) take ownership of the slot for this seq. */
    if (s->occupied && s->seq == seq) {
        return NANORTC_OK;
    }
    s->seq = seq;
    s->ts = ts;
    s->marker = marker;
    s->arrival_ms = now_ms;
    s->len = len;
    memcpy(s->data, data, len);
    s->occupied = true;
    return NANORTC_OK;
}

/* Skip the missing next_seq only if a buffered (future) packet has already
 * waited out the latency cap — otherwise hold and wait for the gap to fill. */
static bool reorder_should_skip(const nano_reorder_t *r, uint32_t now_ms)
{
    for (uint16_t i = 0; i < NANORTC_VIDEO_REORDER_SLOTS; i++) {
        const nano_reorder_slot_t *s = &r->slots[i];
        if (!s->occupied) {
            continue;
        }
        /* Only a packet AHEAD of next_seq within the window, waiting past the
         * cap, forces the gap skip. Exclude stale slots left behind by a
         * force-advance (seq < next_seq): they are already lost and must not
         * trigger a premature skip of a gap that could still be filled. */
        int16_t ahead = (int16_t)(s->seq - r->next_seq);
        if (ahead > 0 && ahead < (int16_t)NANORTC_VIDEO_REORDER_SLOTS &&
            (uint32_t)(now_ms - s->arrival_ms) >= (uint32_t)NANORTC_VIDEO_REORDER_MAX_WAIT_MS) {
            return true;
        }
    }
    return false;
}

int reorder_pop(nano_reorder_t *r, uint32_t now_ms, uint16_t *out_seq, uint32_t *out_ts,
                uint8_t *out_marker, const uint8_t **out_data, size_t *out_len, bool *lost)
{
    if (!r || !out_seq || !out_ts || !out_marker || !out_data || !out_len || !lost) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (!r->inited) {
        return NANORTC_ERR_NO_DATA;
    }

    for (;;) {
        nano_reorder_slot_t *s = &r->slots[reorder_slot_idx(r->next_seq)];
        if (s->occupied && s->seq == r->next_seq) {
            *out_seq = s->seq;
            *out_ts = s->ts;
            *out_marker = s->marker;
            *out_data = s->data;
            *out_len = s->len;
            *lost = r->pending_lost;
            r->pending_lost = false;
            s->occupied = false;
            r->next_seq++;
            return NANORTC_OK;
        }

        /* next_seq is missing. Skip it only once a buffered packet has aged out
         * the latency cap; otherwise wait for it to arrive. */
        if (reorder_should_skip(r, now_ms)) {
            r->next_seq++;
            r->pending_lost = true;
            continue;
        }
        return NANORTC_ERR_NO_DATA;
    }
}

uint32_t reorder_next_timeout_ms(const nano_reorder_t *r, uint32_t now_ms)
{
    if (!r || !r->inited) {
        return UINT32_MAX;
    }
    /* next_seq already buffered -> a pop would release it immediately. */
    const nano_reorder_slot_t *head = &r->slots[reorder_slot_idx(r->next_seq)];
    if (head->occupied && head->seq == r->next_seq) {
        return 0;
    }
    /* Gap at next_seq: wake when the oldest in-window packet ahead of it ages
     * out the cap (which makes reorder_should_skip fire and release the gap). */
    uint32_t best = UINT32_MAX;
    for (uint16_t i = 0; i < NANORTC_VIDEO_REORDER_SLOTS; i++) {
        const nano_reorder_slot_t *s = &r->slots[i];
        if (!s->occupied) {
            continue;
        }
        int16_t ahead = (int16_t)(s->seq - r->next_seq);
        if (ahead > 0 && ahead < (int16_t)NANORTC_VIDEO_REORDER_SLOTS) {
            uint32_t age = now_ms - s->arrival_ms;
            uint32_t left = (age >= (uint32_t)NANORTC_VIDEO_REORDER_MAX_WAIT_MS)
                                ? 0u
                                : ((uint32_t)NANORTC_VIDEO_REORDER_MAX_WAIT_MS - age);
            if (left < best) {
                best = left;
            }
        }
    }
    return best;
}

#endif /* NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_REORDER */
