/*
 * nanortc — Jitter buffer
 *
 * Fixed-size ring buffer indexed by seq % NANORTC_JITTER_SLOTS.
 * Provides playout delay for reordering and smooth output.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_jitter.h"
#include "nano_log.h"
#include "nanortc.h"
#include <string.h>

int jitter_init(nano_jitter_t *jb, uint32_t depth_ms)
{
    if (!jb) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    memset(jb, 0, sizeof(*jb));
    jb->depth_ms = depth_ms;
    NANORTC_LOGI("JITTER", "init ok");
    return NANORTC_OK;
}

int jitter_push(nano_jitter_t *jb, uint16_t seq, uint32_t timestamp, const uint8_t *data,
                size_t len, uint32_t now_ms)
{
    if (!jb || !data || len == 0) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (len > NANORTC_JITTER_SLOT_DATA_SIZE) {
        NANORTC_LOGW("JITTER", "packet too large for slot");
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }

    /* First packet: initialize head_seq */
    if (!jb->started) {
        jb->head_seq = seq;
        jb->started = 1;
    }

    /* Check if packet is too old (behind head_seq by more than buffer size).
     * Use signed 16-bit comparison to handle wraparound. */
    int16_t delta = (int16_t)(seq - jb->head_seq);
    if (delta < 0 && (uint16_t)(-delta) >= NANORTC_JITTER_SLOTS) {
        /* Too old, discard */
        NANORTC_LOGD("JITTER", "discarded stale packet");
        return NANORTC_ERR_NO_DATA;
    }

    /* If packet is far ahead, advance head_seq (clearing skipped slots) */
    if (delta >= (int16_t)NANORTC_JITTER_SLOTS) {
        /* Advance head to make room: new head = seq - NANORTC_JITTER_SLOTS + 1 */
        uint16_t new_head = (uint16_t)(seq - NANORTC_JITTER_SLOTS + 1);
        /* Clear slots that are being skipped over */
        while (jb->head_seq != new_head) {
            uint16_t idx = jb->head_seq % NANORTC_JITTER_SLOTS;
            jb->slots[idx].occupied = 0;
            jb->head_seq++;
        }
    }

    uint16_t idx = seq % NANORTC_JITTER_SLOTS;
    nano_jitter_slot_t *slot = &jb->slots[idx];

    memcpy(slot->data, data, len);
    slot->len = len;
    slot->seq = seq;
    slot->timestamp = timestamp;
    slot->arrival_ms = now_ms;
    slot->occupied = 1;

    return NANORTC_OK;
}

int jitter_pop(nano_jitter_t *jb, uint32_t now_ms, uint8_t *buf, size_t buf_len, size_t *out_len,
               uint32_t *timestamp)
{
    if (!jb || !buf || !out_len) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    if (!jb->started) {
        return NANORTC_ERR_NO_DATA;
    }

    uint16_t idx = jb->head_seq % NANORTC_JITTER_SLOTS;
    nano_jitter_slot_t *slot = &jb->slots[idx];

    if (!slot->occupied) {
        /* Head slot is empty — packet was lost or hasn't arrived yet.
         * Check if enough time has passed to skip it. */
        /* Look ahead: find if any later slot has arrived and its delay has passed */
        /* For simplicity, if head slot is empty and we've waited long enough
         * (based on any occupied slot's arrival time), skip the missing packet. */

        /* Find earliest occupied slot in the buffer to check timing */
        uint8_t found = 0;
        for (uint16_t i = 1; i < NANORTC_JITTER_SLOTS; i++) {
            uint16_t check_seq = (uint16_t)(jb->head_seq + i);
            uint16_t check_idx = check_seq % NANORTC_JITTER_SLOTS;
            nano_jitter_slot_t *check = &jb->slots[check_idx];
            if (check->occupied && check->seq == check_seq) {
                /* If a later packet has been waiting long enough, skip the gap */
                if (now_ms - check->arrival_ms >= jb->depth_ms) {
                    /* Skip missing head packet, advance to this one */
                    jb->head_seq = check_seq;
                    found = 1;
                    break;
                }
                break; /* Not ready yet */
            }
        }

        if (!found) {
            return NANORTC_ERR_NO_DATA;
        }

        /* Re-read with updated head */
        idx = jb->head_seq % NANORTC_JITTER_SLOTS;
        slot = &jb->slots[idx];
    }

    /* Check that this is the right seq (not a stale slot from previous cycle) */
    if (slot->seq != jb->head_seq) {
        slot->occupied = 0;
        jb->head_seq++;
        return NANORTC_ERR_NO_DATA;
    }

    /* Enforce playout delay */
    if (now_ms - slot->arrival_ms < jb->depth_ms) {
        return NANORTC_ERR_NO_DATA;
    }

    /* Output the packet */
    if (buf_len < slot->len) {
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }

    memcpy(buf, slot->data, slot->len);
    *out_len = slot->len;
    if (timestamp) {
        *timestamp = slot->timestamp;
    }

    slot->occupied = 0;
    jb->head_seq++;

    return NANORTC_OK;
}
