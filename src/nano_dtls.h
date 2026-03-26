/*
 * nanortc — DTLS state machine internal interface
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANO_DTLS_H_
#define NANO_DTLS_H_

#include <stdint.h>
#include <stddef.h>

typedef enum {
    NANO_DTLS_STATE_INIT,
    NANO_DTLS_STATE_HANDSHAKING,
    NANO_DTLS_STATE_ESTABLISHED,
    NANO_DTLS_STATE_CLOSED,
} nano_dtls_state_t;

typedef struct nano_dtls {
    nano_dtls_state_t state;

    /* Outbound: DTLS records to send (filled by crypto provider) */
    uint8_t out_buf[2048];
    size_t out_len;

    /* Inbound: received DTLS records */
    uint8_t in_buf[2048];
    size_t in_len;

    /* Crypto provider context placeholder */
    void *crypto_ctx;
} nano_dtls_t;

int dtls_init(nano_dtls_t *dtls);
int dtls_handle_data(nano_dtls_t *dtls, const uint8_t *data, size_t len);
int dtls_poll_output(nano_dtls_t *dtls, uint8_t *buf, size_t buf_len, size_t *out_len);

#endif /* NANO_DTLS_H_ */
