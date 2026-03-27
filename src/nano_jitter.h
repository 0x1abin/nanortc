/*
 * nanortc — Jitter buffer internal interface
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANO_JITTER_H_
#define NANO_JITTER_H_

#include "nanortc_config.h"

#include <stdint.h>
#include <stddef.h>

/* Maximum RTP packet data per jitter slot */
#define NANO_JITTER_SLOT_DATA_SIZE 1500

typedef struct nano_jitter_slot {
    uint8_t data[NANO_JITTER_SLOT_DATA_SIZE];
    size_t len;
    uint16_t seq;
    uint32_t timestamp;
    uint8_t occupied;
} nano_jitter_slot_t;

typedef struct nano_jitter {
    nano_jitter_slot_t slots[NANO_JITTER_SLOTS];
    uint16_t head_seq;
    uint32_t depth_ms;
} nano_jitter_t;

int jitter_init(nano_jitter_t *jb, uint32_t depth_ms);
int jitter_push(nano_jitter_t *jb, uint16_t seq, uint32_t timestamp, const uint8_t *data,
                size_t len);
int jitter_pop(nano_jitter_t *jb, uint32_t now_ms, uint8_t *buf, size_t buf_len, size_t *out_len,
               uint32_t *timestamp);

#endif /* NANO_JITTER_H_ */
