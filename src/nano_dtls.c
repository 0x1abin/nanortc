/*
 * nanortc — DTLS state machine (RFC 6347)
 *
 * Sans I/O BIO adapter: bridges NanoRTC's buffer-based event loop
 * with crypto provider (mbedtls/OpenSSL) DTLS implementations.
 *
 * Data flow:
 *   1. dtls_handle_data() → copies network bytes into in_buf
 *   2. Calls crypto->dtls_handshake() or crypto->dtls_decrypt()
 *   3. Provider's BIO recv callback reads from in_buf
 *   4. Provider's BIO send callback writes to out_buf
 *   5. dtls_poll_output() → returns data from out_buf
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_dtls.h"
#include "nanortc_crypto.h"
#include "nanortc.h"
#include <string.h>

/* ----------------------------------------------------------------
 * BIO callbacks — registered with crypto provider via dtls_set_bio
 * ---------------------------------------------------------------- */

/*
 * Called by the crypto provider when it wants to send a DTLS record.
 * Appends data to dtls->out_buf.
 */
static int bio_send_cb(void *userdata, const uint8_t *data, size_t len)
{
    nano_dtls_t *dtls = (nano_dtls_t *)userdata;
    if (dtls->out_len + len > NANORTC_DTLS_BUF_SIZE) {
        return -1; /* Buffer full */
    }
    memcpy(dtls->out_buf + dtls->out_len, data, len);
    dtls->out_len += len;
    return (int)len;
}

/*
 * Called by the crypto provider when it wants to read a DTLS record.
 * Returns data from dtls->in_buf, or 0 if empty (provider treats as WANT_READ).
 */
static int bio_recv_cb(void *userdata, uint8_t *buf, size_t buf_len)
{
    nano_dtls_t *dtls = (nano_dtls_t *)userdata;
    size_t avail = dtls->in_len - dtls->in_read_pos;
    if (avail == 0) {
        return 0; /* No data — provider interprets as WANT_READ */
    }
    size_t to_copy = avail < buf_len ? avail : buf_len;
    memcpy(buf, dtls->in_buf + dtls->in_read_pos, to_copy);
    dtls->in_read_pos += to_copy;
    /* Reset when all consumed */
    if (dtls->in_read_pos >= dtls->in_len) {
        dtls->in_len = 0;
        dtls->in_read_pos = 0;
    }
    return (int)to_copy;
}

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

int dtls_init(nano_dtls_t *dtls, const nanortc_crypto_provider_t *crypto, int is_server)
{
    if (!dtls || !crypto || !crypto->dtls_ctx_new) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    memset(dtls, 0, sizeof(*dtls));
    dtls->state = NANORTC_DTLS_STATE_INIT;
    dtls->crypto = crypto;
    dtls->is_server = is_server;

    /* Allocate crypto context (heap-allocated by provider) */
    nanortc_crypto_dtls_ctx_t *ctx = crypto->dtls_ctx_new(is_server);
    if (!ctx) {
        dtls->state = NANORTC_DTLS_STATE_ERROR;
        return NANORTC_ERR_CRYPTO;
    }
    dtls->crypto_ctx = ctx;

    /* Register BIO callbacks */
    int rc = crypto->dtls_set_bio(ctx, dtls, bio_send_cb, bio_recv_cb);
    if (rc != 0) {
        crypto->dtls_free(ctx);
        dtls->crypto_ctx = NULL;
        dtls->state = NANORTC_DTLS_STATE_ERROR;
        return NANORTC_ERR_CRYPTO;
    }

    /* Cache local fingerprint */
    crypto->dtls_get_fingerprint(ctx, dtls->local_fingerprint, sizeof(dtls->local_fingerprint));

    return NANORTC_OK;
}

int dtls_start(nano_dtls_t *dtls)
{
    if (!dtls || !dtls->crypto || !dtls->crypto_ctx) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (dtls->state != NANORTC_DTLS_STATE_INIT) {
        return NANORTC_ERR_STATE;
    }

    dtls->state = NANORTC_DTLS_STATE_HANDSHAKING;

    /* Drive the handshake — for client, this generates ClientHello */
    int rc = dtls->crypto->dtls_handshake((nanortc_crypto_dtls_ctx_t *)dtls->crypto_ctx);
    if (rc < 0) {
        dtls->state = NANORTC_DTLS_STATE_ERROR;
        return NANORTC_ERR_CRYPTO;
    }
    /* rc == 0: handshake done (unlikely on first call)
     * rc == 1: WANT_READ — ClientHello is in out_buf, waiting for ServerHello */

    if (rc == 0) {
        dtls->state = NANORTC_DTLS_STATE_ESTABLISHED;
    }

    return NANORTC_OK;
}

int dtls_handle_data(nano_dtls_t *dtls, const uint8_t *data, size_t len)
{
    if (!dtls || !data || len == 0) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (!dtls->crypto || !dtls->crypto_ctx) {
        return NANORTC_ERR_STATE;
    }

    /* Buffer incoming data */
    if (dtls->in_len + len > NANORTC_DTLS_BUF_SIZE) {
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(dtls->in_buf + dtls->in_len, data, len);
    dtls->in_len += len;

    nanortc_crypto_dtls_ctx_t *ctx = (nanortc_crypto_dtls_ctx_t *)dtls->crypto_ctx;

    if (dtls->state == NANORTC_DTLS_STATE_INIT || dtls->state == NANORTC_DTLS_STATE_HANDSHAKING) {
        /* Handshake phase: drive the handshake forward */
        if (dtls->state == NANORTC_DTLS_STATE_INIT) {
            dtls->state = NANORTC_DTLS_STATE_HANDSHAKING;
        }

        int rc = dtls->crypto->dtls_handshake(ctx);
        if (rc < 0) {
            dtls->state = NANORTC_DTLS_STATE_ERROR;
            return NANORTC_ERR_CRYPTO;
        }
        if (rc == 0) {
            /* Handshake complete */
            dtls->state = NANORTC_DTLS_STATE_ESTABLISHED;

            /* Export keying material (RFC 5764) */
            if (dtls->crypto->dtls_export_keying_material) {
                int km_rc = dtls->crypto->dtls_export_keying_material(
                    ctx, "EXTRACTOR-dtls_srtp", sizeof("EXTRACTOR-dtls_srtp") - 1,
                    dtls->keying_material, sizeof(dtls->keying_material));
                if (km_rc == 0) {
                    dtls->keying_material_ready = 1;
                }
            }
        }
        return NANORTC_OK;

    } else if (dtls->state == NANORTC_DTLS_STATE_ESTABLISHED) {
        /* Application data: decrypt */
        dtls->app_len = NANORTC_DTLS_BUF_SIZE;
        int rc = dtls->crypto->dtls_decrypt(ctx, NULL, 0, dtls->app_buf, &dtls->app_len);
        if (rc < 0) {
            dtls->app_len = 0;
            return NANORTC_ERR_CRYPTO;
        }
        /* rc == 1 means WANT_READ (no app data yet, could be handshake renegotiation) */
        /* Drain any outbound data generated (e.g., close_notify response) */
        return NANORTC_OK;
    }

    return NANORTC_ERR_STATE;
}

int dtls_poll_output(nano_dtls_t *dtls, uint8_t *buf, size_t buf_len, size_t *out_len)
{
    if (!dtls || !buf || !out_len) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    if (dtls->out_len == 0) {
        *out_len = 0;
        return NANORTC_ERR_NO_DATA;
    }

    if (buf_len < dtls->out_len) {
        *out_len = 0;
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }

    memcpy(buf, dtls->out_buf, dtls->out_len);
    *out_len = dtls->out_len;
    dtls->out_len = 0;
    return NANORTC_OK;
}

int dtls_encrypt(nano_dtls_t *dtls, const uint8_t *in, size_t in_len)
{
    if (!dtls || !in || in_len == 0) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (dtls->state != NANORTC_DTLS_STATE_ESTABLISHED) {
        return NANORTC_ERR_STATE;
    }

    nanortc_crypto_dtls_ctx_t *ctx = (nanortc_crypto_dtls_ctx_t *)dtls->crypto_ctx;
    int rc = dtls->crypto->dtls_encrypt(ctx, in, in_len, NULL, NULL);
    if (rc < 0) {
        return NANORTC_ERR_CRYPTO;
    }
    /* Encrypted DTLS record is now in out_buf via BIO send callback */
    return NANORTC_OK;
}

int dtls_poll_app_data(nano_dtls_t *dtls, const uint8_t **data, size_t *len)
{
    if (!dtls || !data || !len) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (dtls->app_len == 0) {
        *data = NULL;
        *len = 0;
        return NANORTC_ERR_NO_DATA;
    }
    *data = dtls->app_buf;
    *len = dtls->app_len;
    dtls->app_len = 0; /* Consumed */
    return NANORTC_OK;
}

const char *dtls_get_fingerprint(nano_dtls_t *dtls)
{
    if (!dtls || dtls->local_fingerprint[0] == '\0') {
        return NULL;
    }
    return dtls->local_fingerprint;
}

void dtls_destroy(nano_dtls_t *dtls)
{
    if (!dtls) {
        return;
    }
    if (dtls->crypto && dtls->crypto_ctx) {
        dtls->crypto->dtls_free((nanortc_crypto_dtls_ctx_t *)dtls->crypto_ctx);
    }
    dtls->crypto_ctx = NULL;
    dtls->state = NANORTC_DTLS_STATE_CLOSED;
}
