/*
 * nanortc — DTLS state machine internal interface (RFC 6347)
 * @internal Not part of the public API.
 *
 * Sans I/O BIO adapter: bridges crypto providers (mbedtls/OpenSSL)
 * with NanoRTC's buffer-based event loop.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_DTLS_H_
#define NANORTC_DTLS_H_

#include "nanortc_config.h"
#include "nanortc_crypto.h"

#include <stdint.h>
#include <stddef.h>

/* SRTP keying material size (RFC 5764 §4.2):
 * client_key(16) + server_key(16) + client_salt(14) + server_salt(14) */
#define NANORTC_DTLS_KEYING_SIZE 60

/* DTLS record header (RFC 6347 §4.1): type(1) + version(2) + epoch(2) + seq(6) + length(2) = 13.
 * PREFIX_SIZE = first 11 bytes, everything before the length field —
 * preserved verbatim when synthesizing a reassembled ClientHello record. */
#define NANORTC_DTLS_RECORD_HDR_PREFIX_SIZE 11

typedef enum {
    NANORTC_DTLS_STATE_INIT,
    NANORTC_DTLS_STATE_HANDSHAKING,
    NANORTC_DTLS_STATE_ESTABLISHED,
    NANORTC_DTLS_STATE_CLOSED,
    NANORTC_DTLS_STATE_ERROR,
} nano_dtls_state_t;

typedef struct nano_dtls {
    nano_dtls_state_t state;

    /* Outbound: DTLS records to send (filled by crypto provider via BIO send) */
    uint8_t out_buf[NANORTC_DTLS_BUF_SIZE];
    size_t out_len;

    /* Inbound: received DTLS records from network */
    uint8_t in_buf[NANORTC_DTLS_BUF_SIZE];
    size_t in_len;
    size_t in_read_pos; /* consumed position by crypto provider recv */

    /* Crypto provider context (heap-allocated by provider, opaque to src/) */
    void *crypto_ctx;

    /* Pointer to crypto provider for calling DTLS ops */
    const nanortc_crypto_provider_t *crypto;

    /* Role: 0 = client (active/offerer), 1 = server (passive/answerer) */
    int is_server;

    /* Post-handshake: SRTP keying material (RFC 5764 §4.2)
     * 60 bytes: client_key(16) + server_key(16) + client_salt(14) + server_salt(14) */
    uint8_t keying_material[NANORTC_DTLS_KEYING_SIZE];
    int keying_material_ready;

    /* Decrypted application data (SCTP packets after DTLS decrypt) */
    uint8_t app_buf[NANORTC_DTLS_BUF_SIZE];
    size_t app_len;

    /* Local certificate fingerprint (SHA-256, "XX:XX:..." format, 95 chars + NUL) */
    char local_fingerprint[NANORTC_DTLS_FINGERPRINT_STR_SIZE];

    /* ClientHello fragment reassembly (server-side only, workaround for
     * mbedtls 3.6 rejecting fragmented ClientHello at ssl_tls12_server.c:1099).
     * Chrome's DTLS ClientHello with X25519MLKEM768 post-quantum key_share
     * runs ~1.4 KB and fragments into 2 DTLS records over the 1200 B MTU.
     * We buffer fragments (body bytes written into app_buf — harmless because
     * app_buf is only used once DTLS reaches ESTABLISHED, which comes after
     * the whole CHLO has been consumed) then hand mbedtls a synthesized
     * single-record unfragmented ClientHello built into in_buf.
     *
     * chlo_total == 0 means idle. */
    uint32_t chlo_total;   /* expected ClientHello body length; 0 = idle */
    uint32_t chlo_have;    /* bytes of body assembled so far */
    uint16_t chlo_msg_seq; /* handshake msg_seq (must match across fragments) */
    uint8_t chlo_rec_hdr[NANORTC_DTLS_RECORD_HDR_PREFIX_SIZE]; /* saved bytes 0..10 of record
                                                                  (type/ver/epoch/seq) */
} nano_dtls_t;

/*
 * Initialize DTLS state machine and create crypto context.
 * is_server: 1 = DTLS server (answerer), 0 = DTLS client (offerer).
 */
int dtls_init(nano_dtls_t *dtls, const nanortc_crypto_provider_t *crypto, int is_server);

/*
 * Start handshake (client role: generates ClientHello).
 * After this call, poll dtls_poll_output() for the initial flight.
 */
int dtls_start(nano_dtls_t *dtls);

/*
 * Feed incoming DTLS record from network.
 * Returns NANORTC_OK on success, negative on error.
 * Check dtls->state for transitions; poll dtls_poll_output() for responses.
 */
int dtls_handle_data(nano_dtls_t *dtls, const uint8_t *data, size_t len);

/*
 * Server-side ClientHello fragment reassembly (workaround for mbedtls 3.6
 * refusing fragmented ClientHello at ssl_tls12_server.c:1099). Called from
 * dtls_handle_data(); exposed here only so unit tests can exercise it
 * directly. Not intended for external callers.
 *
 * Returns:
 *   1  — fragment buffered, or full ClientHello synthesized into dtls->in_buf
 *   0  — not a fragmented ClientHello, caller should pass through normally
 *  <0  — malformed input, drop
 */
int dtls_try_reassemble_chlo(nano_dtls_t *dtls, const uint8_t *data, size_t len);

/*
 * Poll for outbound DTLS records.
 * Returns NANORTC_OK and sets *out_len if data available, NANORTC_ERR_NO_DATA if empty.
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
 * Returns NANORTC_OK if app data available, NANORTC_ERR_NO_DATA otherwise.
 */
int dtls_poll_app_data(nano_dtls_t *dtls, const uint8_t **data, size_t *len);

/* Get local certificate fingerprint (available after dtls_init). */
const char *dtls_get_fingerprint(nano_dtls_t *dtls);

/*
 * Initiate graceful close (sends close_notify if supported).
 * Sets state to CLOSED. Poll dtls_poll_output() for the close record.
 */
void dtls_close(nano_dtls_t *dtls);

/* Release crypto context and reset state. */
void dtls_destroy(nano_dtls_t *dtls);

#endif /* NANORTC_DTLS_H_ */
