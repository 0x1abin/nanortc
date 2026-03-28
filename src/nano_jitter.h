/*
 * nanortc — Jitter buffer internal interface
 *
 * Fixed-size ring buffer indexed by RTP sequence number.
 * Reorders out-of-order packets and adds playout delay.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_JITTER_H_
#define NANORTC_JITTER_H_

#include "nanortc_config.h"

#include <stdint.h>
#include <stddef.h>

typedef struct nano_jitter_slot {
    uint8_t data[NANORTC_JITTER_SLOT_DATA_SIZE];
    size_t len;
    uint16_t seq;
    uint32_t timestamp;
    uint32_t arrival_ms; /* Local time when packet arrived */
    uint8_t occupied;
} nano_jitter_slot_t;

typedef struct nano_jitter {
    nano_jitter_slot_t slots[NANORTC_JITTER_SLOTS];
    uint16_t head_seq; /* Next sequence number to pop */
    uint32_t depth_ms; /* Playout delay */
    uint8_t started;   /* True after first push */
} nano_jitter_t;

int jitter_init(nano_jitter_t *jb, uint32_t depth_ms);
int jitter_push(nano_jitter_t *jb, uint16_t seq, uint32_t timestamp, const uint8_t *data,
                size_t len, uint32_t now_ms);
int jitter_pop(nano_jitter_t *jb, uint32_t now_ms, uint8_t *buf, size_t buf_len, size_t *out_len,
               uint32_t *timestamp);

#endif /* NANORTC_JITTER_H_ */
