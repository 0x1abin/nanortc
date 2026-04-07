/*
 * Fuzz harness for STUN parser — nano_stun.c
 *
 * Targets: stun_parse(), stun_verify_fingerprint()
 * Attack surface: First parser hit by any inbound UDP packet.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_stun.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    stun_msg_t msg;
    memset(&msg, 0, sizeof(msg));

    /* Parse arbitrary bytes as a STUN message */
    stun_parse(data, size, &msg);

    /* Also exercise fingerprint verification on the same data */
    stun_verify_fingerprint(data, size);

    return 0;
}
