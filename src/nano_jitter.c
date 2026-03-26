/*
 * nanortc — Jitter buffer
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_jitter.h"
#include <string.h>

int jitter_init(nano_jitter_t *jb, uint32_t depth_ms)
{
    if (!jb) {
        return -1;
    }
    memset(jb, 0, sizeof(*jb));
    jb->depth_ms = depth_ms;
    return 0;
}

int jitter_push(nano_jitter_t *jb, uint16_t seq, uint32_t timestamp,
                const uint8_t *data, size_t len)
{
    (void)jb;
    (void)seq;
    (void)timestamp;
    (void)data;
    (void)len;
    return -1;
}

int jitter_pop(nano_jitter_t *jb, uint32_t now_ms,
               uint8_t *buf, size_t buf_len, size_t *out_len, uint32_t *timestamp)
{
    (void)jb;
    (void)now_ms;
    (void)buf;
    (void)buf_len;
    (void)out_len;
    (void)timestamp;
    return -1;
}
