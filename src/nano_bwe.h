/*
 * nanortc — Bandwidth estimation internal interface (MEDIA profile)
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_BWE_H_
#define NANORTC_BWE_H_

#include <stdint.h>
#include <stddef.h>

typedef struct nano_bwe {
    uint32_t estimated_bitrate; /* bps */
    uint32_t last_update_ms;
} nano_bwe_t;

int bwe_init(nano_bwe_t *bwe);
int bwe_on_rtcp_feedback(nano_bwe_t *bwe, const uint8_t *data, size_t len, uint32_t now_ms);
uint32_t bwe_get_bitrate(const nano_bwe_t *bwe);

#endif /* NANORTC_BWE_H_ */
