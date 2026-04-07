/*
 * Fuzz harness for bandwidth estimation — nano_bwe.c
 *
 * Targets: bwe_parse_remb(), bwe_on_rtcp_feedback()
 * Attack surface: REMB binary parsing + stateful feedback processing.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_bwe.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* Stateless: REMB parsing */
    uint32_t bitrate = 0;
    bwe_parse_remb(data, size, &bitrate);

    /* Stateful: RTCP feedback handling */
    nano_bwe_t bwe;
    memset(&bwe, 0, sizeof(bwe));
    bwe_init(&bwe);

    bwe_on_rtcp_feedback(&bwe, data, size, 1000);

    return 0;
}
