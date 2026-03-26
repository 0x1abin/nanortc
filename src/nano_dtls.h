/*
 * nanortc — DTLS state machine internal interface (RFC 6347)
 *
 * Sans I/O BIO adapter: bridges crypto providers (mbedtls/OpenSSL)
 * with NanoRTC's buffer-based event loop.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANO_DTLS_H_
#define NANO_DTLS_H_

#include <stdint.h>
#include <stddef.h>

/* Forward declaration — defined in nanortc.h */
#ifndef NANO_DTLS_BUF_SIZE
#define NANO_DTLS_BUF_SIZE 2048
#endif

/* Forward declaration */
typedef struct nano_crypto_provider nano_crypto_provider_t;

typedef enum {
    NANO_DTLS_STATE_INIT,
    NANO_DTLS_STATE_HANDSHAKING,
    NANO_DTLS_STATE_ESTABLISHED,
    NANO_DTLS_STATE_CLOSED,
    NANO_DTLS_STATE_ERROR,
} nano_dtls_state_t;

typedef struct nano_dtls {
    nano_dtls_state_t state;

    /* Outbound: DTLS records to send (filled by crypto provider via BIO send) */
    uint8_t out_buf[NANO_DTLS_BUF_SIZE];
    size_t out_len;

    /* Inbound: received DTLS records from network */
    uint8_t in_buf[NANO_DTLS_BUF_SIZE];
    size_t in_len;
    size_t in_read_pos; /* consumed position by crypto provider recv */

    /* Crypto provider context (heap-allocated by provider, opaque to src/) */
    void *crypto_ctx;

    /* Pointer to crypto provider for calling DTLS ops */
    const nano_crypto_provider_t *crypto;

    /* Role: 0 = client (active/offerer), 1 = server (passive/answerer) */
    int is_server;

    /* Post-handshake: SRTP keying material (RFC 5764 §4.2)
     * 60 bytes: client_key(16) + server_key(16) + client_salt(14) + server_salt(14) */
    uint8_t keying_material[60];
    int keying_material_ready;

    /* Decrypted application data (SCTP packets after DTLS decrypt) */
    uint8_t app_buf[NANO_DTLS_BUF_SIZE];
    size_t app_len;

    /* Local certificate fingerprint (SHA-256, "XX:XX:..." format, 95 chars + NUL) */
    char local_fingerprint[97];
} nano_dtls_t;

/*
 * Initialize DTLS state machine and create crypto context.
 * is_server: 1 = DTLS server (answerer), 0 = DTLS client (offerer).
 */
int dtls_init(nano_dtls_t *dtls, const nano_crypto_provider_t *crypto, int is_server);

/*
 * Start handshake (client role: generates ClientHello).
 * After this call, poll dtls_poll_output() for the initial flight.
 */
int dtls_start(nano_dtls_t *dtls);

/*
 * Feed incoming DTLS record from network.
 * Returns NANO_OK on success, negative on error.
 * Check dtls->state for transitions; poll dtls_poll_output() for responses.
 */
int dtls_handle_data(nano_dtls_t *dtls, const uint8_t *data, size_t len);

/*
 * Poll for outbound DTLS records.
 * Returns NANO_OK and sets *out_len if data available, NANO_ERR_NO_DATA if empty.
 */
int dtls_poll_output(nano_dtls_t *dtls, uint8_t *buf, size_t buf_len, size_t *out_len);

/*
 * Encrypt application data (post-handshake). Output is a DTLS record.
 * Poll dtls_poll_output() for the encrypted record.
 */
int dtls_encrypt(nano_dtls_t *dtls, const uint8_t *in, size_t in_len);

/*
 * Get decrypted application data after dtls_handle_data() on an established
 * connection. Returns pointer and length via dtls->app_buf / dtls->app_len.
 * Returns NANO_OK if app data available, NANO_ERR_NO_DATA otherwise.
 */
int dtls_poll_app_data(nano_dtls_t *dtls, const uint8_t **data, size_t *len);

/* Get local certificate fingerprint (available after dtls_init). */
const char *dtls_get_fingerprint(nano_dtls_t *dtls);

/* Release crypto context and reset state. */
void dtls_destroy(nano_dtls_t *dtls);

#endif /* NANO_DTLS_H_ */
