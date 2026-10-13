/*
 * Fuzz harness for bandwidth estimation — nano_bwe.c
 *
 * Targets: REMB/TWCC parsing, actual-send history and feedback processing.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_bwe.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

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

    /* Exercise feedback against actual departure history, including timestamp
     * wrap, out-of-order departure and exact-ciphertext retransmissions. */
    uint32_t now = size >= 4 ? ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
                                   ((uint32_t)data[2] << 8) | data[3]
                             : 0;
    for (size_t i = 0; i < size && i < 32; i++) {
        now += data[i];
        bwe_record_transmit(&bwe, (uint32_t)(i ^ (data[i] & 1u)), (uint16_t)(data[i] + 100u), now,
                            true, (data[i] & 2u) != 0);
    }
    nano_bwe_t before = bwe;
    int rc = bwe_on_twcc_feedback(&bwe, data, size, now + 100u);
    assert(rc == 0 || memcmp(&bwe, &before, sizeof(bwe)) == 0);

    return 0;
}
