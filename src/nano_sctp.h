/*
 * nanortc — SCTP-Lite state machine internal interface
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANO_SCTP_H_
#define NANO_SCTP_H_

#include <stdint.h>
#include <stddef.h>

typedef enum {
    NANO_SCTP_STATE_CLOSED,
    NANO_SCTP_STATE_COOKIE_WAIT,
    NANO_SCTP_STATE_COOKIE_ECHOED,
    NANO_SCTP_STATE_ESTABLISHED,
    NANO_SCTP_STATE_SHUTDOWN_PENDING,
    NANO_SCTP_STATE_SHUTDOWN_SENT,
    NANO_SCTP_STATE_SHUTDOWN_RECEIVED,
    NANO_SCTP_STATE_SHUTDOWN_ACK_SENT,
} nano_sctp_state_t;

typedef struct nano_sctp {
    nano_sctp_state_t state;
    uint32_t local_vtag;
    uint32_t remote_vtag;
    uint32_t local_tsn;
    uint32_t remote_tsn;
    uint16_t local_port;
    uint16_t remote_port;
} nano_sctp_t;

int sctp_init(nano_sctp_t *sctp);
int sctp_handle_data(nano_sctp_t *sctp, const uint8_t *data, size_t len);
int sctp_poll_output(nano_sctp_t *sctp, uint8_t *buf, size_t buf_len, size_t *out_len);
int sctp_send(nano_sctp_t *sctp, uint16_t stream_id, uint32_t ppid,
              const uint8_t *data, size_t len);

#endif /* NANO_SCTP_H_ */
