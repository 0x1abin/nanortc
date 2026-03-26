/*
 * nanortc — DTLS state machine (RFC 6347)
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_dtls.h"
#include <string.h>

int dtls_init(nano_dtls_t *dtls)
{
    if (!dtls) {
        return -1;
    }
    memset(dtls, 0, sizeof(*dtls));
    dtls->state = NANO_DTLS_STATE_INIT;
    return 0;
}

int dtls_handle_data(nano_dtls_t *dtls, const uint8_t *data, size_t len)
{
    (void)dtls;
    (void)data;
    (void)len;
    return -1;
}

int dtls_poll_output(nano_dtls_t *dtls, uint8_t *buf, size_t buf_len, size_t *out_len)
{
    (void)dtls;
    (void)buf;
    (void)buf_len;
    (void)out_len;
    return -1;
}
