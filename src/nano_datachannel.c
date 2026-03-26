/*
 * nanortc — DataChannel / DCEP protocol (RFC 8832)
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_datachannel.h"
#include <string.h>

int dc_init(nano_dc_t *dc)
{
    if (!dc) {
        return -1;
    }
    memset(dc, 0, sizeof(*dc));
    return 0;
}

int dc_handle_dcep(nano_dc_t *dc, uint16_t stream_id,
                   const uint8_t *data, size_t len)
{
    (void)dc;
    (void)stream_id;
    (void)data;
    (void)len;
    return -1;
}

int dc_open(nano_dc_t *dc, uint16_t stream_id, const char *label,
            uint8_t *out_buf, size_t *out_len)
{
    (void)dc;
    (void)stream_id;
    (void)label;
    (void)out_buf;
    (void)out_len;
    return -1;
}
