/*
 * Fuzz harness for SDP parser — nano_sdp.c
 *
 * Targets: sdp_parse()
 * Attack surface: Text protocol parser, classic fuzz surface with
 *                 line-by-line parsing and attribute extraction.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_sdp.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    nano_sdp_t sdp;
    memset(&sdp, 0, sizeof(sdp));
    sdp_init(&sdp);

    /* sdp_parse expects (char*, size_t) — cast is safe for fuzzing */
    sdp_parse(&sdp, (const char *)data, size);

    return 0;
}
