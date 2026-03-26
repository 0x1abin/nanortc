/*
 * nanortc — SDP parser/generator (RFC 8866)
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_sdp.h"
#include <string.h>

int sdp_init(nano_sdp_t *sdp)
{
    if (!sdp) {
        return -1;
    }
    memset(sdp, 0, sizeof(*sdp));
    sdp->local_sctp_port = 5000;
    return 0;
}

int sdp_parse(nano_sdp_t *sdp, const char *sdp_str, size_t len)
{
    (void)sdp;
    (void)sdp_str;
    (void)len;
    return -1;
}

int sdp_generate_answer(nano_sdp_t *sdp, char *buf, size_t buf_len, size_t *out_len)
{
    (void)sdp;
    (void)buf;
    (void)buf_len;
    (void)out_len;
    return -1;
}
