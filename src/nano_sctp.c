/*
 * nanortc — SCTP-Lite state machine (RFC 4960 subset)
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_sctp.h"
#include <string.h>

int sctp_init(nano_sctp_t *sctp)
{
    if (!sctp) {
        return -1;
    }
    memset(sctp, 0, sizeof(*sctp));
    sctp->state = NANO_SCTP_STATE_CLOSED;
    return 0;
}

int sctp_handle_data(nano_sctp_t *sctp, const uint8_t *data, size_t len)
{
    (void)sctp;
    (void)data;
    (void)len;
    return -1;
}

int sctp_poll_output(nano_sctp_t *sctp, uint8_t *buf, size_t buf_len, size_t *out_len)
{
    (void)sctp;
    (void)buf;
    (void)buf_len;
    (void)out_len;
    return -1;
}

int sctp_send(nano_sctp_t *sctp, uint16_t stream_id, uint32_t ppid,
              const uint8_t *data, size_t len)
{
    (void)sctp;
    (void)stream_id;
    (void)ppid;
    (void)data;
    (void)len;
    return -1;
}
