/*
 * nanortc — Bandwidth estimation (MEDIA profile only)
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_bwe.h"
#include <string.h>

int bwe_init(nano_bwe_t *bwe)
{
    if (!bwe) {
        return -1;
    }
    memset(bwe, 0, sizeof(*bwe));
    bwe->estimated_bitrate = 300000; /* initial 300 kbps */
    return 0;
}

int bwe_on_rtcp_feedback(nano_bwe_t *bwe, const uint8_t *data, size_t len,
                         uint32_t now_ms)
{
    (void)bwe;
    (void)data;
    (void)len;
    (void)now_ms;
    return -1;
}

uint32_t bwe_get_bitrate(const nano_bwe_t *bwe)
{
    if (!bwe) {
        return 0;
    }
    return bwe->estimated_bitrate;
}
