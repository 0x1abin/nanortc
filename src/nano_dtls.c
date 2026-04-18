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
#include "nano_log.h"
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

/*
 * Server-side workaround for mbedtls 3.6's refusal of fragmented ClientHello
 * (ssl_tls12_server.c:1099). Chrome's DTLS ClientHello with post-quantum
 * key_share exceeds MTU and arrives as two DTLS handshake-layer fragments.
 *
 * Buffers fragments, then when the ClientHello body is complete, writes a
 * single synthesized DTLS record (fragment_offset=0, fragment_length=total)
 * into dtls->in_buf for mbedtls to consume.
 *
 * Returns:
 *   1  = buffered a fragment / emitted synth record into in_buf (caller must
 *        still drive the handshake forward)
 *   0  = not a ClientHello fragment; caller should fall through to normal
 *        pass-through buffering
 *  <0  = sanity-check failure; drop the packet
 */
static int dtls_try_reassemble_chlo(nano_dtls_t *dtls, const uint8_t *data, size_t len)
{
    /* Only act on server side and only during INIT/HANDSHAKING. */
    if (!dtls->is_server)
        return 0;
    if (dtls->state != NANORTC_DTLS_STATE_INIT && dtls->state != NANORTC_DTLS_STATE_HANDSHAKING)
        return 0;
    /* DTLS record hdr (13) + handshake hdr (12) = 25 minimum. */
    if (len < 25)
        return 0;
    if (data[0] != 0x16) /* ContentType: handshake */
        return 0;
    uint16_t rec_len = ((uint16_t)data[11] << 8) | data[12];
    if ((size_t)rec_len + 13 > len || rec_len < 12)
        return 0;
    if (data[13] != 0x01) /* HandshakeType: ClientHello */
        return 0;

    uint32_t total = ((uint32_t)data[14] << 16) | ((uint32_t)data[15] << 8) | (uint32_t)data[16];
    uint16_t msg_seq = ((uint16_t)data[17] << 8) | data[18];
    uint32_t frag_off = ((uint32_t)data[19] << 16) | ((uint32_t)data[20] << 8) | (uint32_t)data[21];
    uint32_t frag_len = ((uint32_t)data[22] << 16) | ((uint32_t)data[23] << 8) | (uint32_t)data[24];

    if (frag_off == 0 && frag_len == total) {
        /* Single-record ClientHello, no reassembly needed. */
        return 0;
    }

    /* Fragment detected. Sanity-bound total length and offsets. CHLO body is
     * parked in app_buf, which is unused until DTLS state reaches ESTABLISHED
     * (see `dtls_decrypt` path); this function only runs in INIT/HANDSHAKING
     * so the two uses never overlap. */
    if (total > sizeof(dtls->app_buf))
        return -1;
    if ((uint32_t)frag_off + frag_len > total)
        return -1;
    if ((size_t)25 + frag_len > len)
        return -1;

    /* Start a fresh assembly if first fragment or retransmission of a
     * different ClientHello. */
    if (dtls->chlo_total == 0 || dtls->chlo_total != total || dtls->chlo_msg_seq != msg_seq) {
        dtls->chlo_total = total;
        dtls->chlo_have = 0;
        dtls->chlo_msg_seq = msg_seq;
        memcpy(dtls->chlo_rec_hdr, data, NANORTC_DTLS_RECORD_HDR_PREFIX_SIZE);
    }

    /* Copy fragment body at its declared offset. Chrome sends in order but
     * tolerate out-of-order: we track contiguous coverage from 0. */
    memcpy(dtls->app_buf + frag_off, &data[25], frag_len);
    if (frag_off + frag_len > dtls->chlo_have)
        dtls->chlo_have = frag_off + frag_len;

    if (dtls->chlo_have < dtls->chlo_total)
        return 1; /* wait for more fragments */

    /* Fully assembled — emit single synthesized DTLS record into in_buf. */
    size_t rec_body_len = (size_t)12 + dtls->chlo_total;
    size_t new_rec_len = 13 + rec_body_len;
    if (dtls->in_len + new_rec_len > NANORTC_DTLS_BUF_SIZE) {
        dtls->chlo_total = 0;
        dtls->chlo_have = 0;
        return -1;
    }

    uint8_t *p = dtls->in_buf + dtls->in_len;
    memcpy(p, dtls->chlo_rec_hdr, NANORTC_DTLS_RECORD_HDR_PREFIX_SIZE);
    p[11] = (uint8_t)(rec_body_len >> 8);
    p[12] = (uint8_t)(rec_body_len & 0xFF);
    p[13] = 0x01; /* ClientHello */
    p[14] = (uint8_t)(dtls->chlo_total >> 16);
    p[15] = (uint8_t)(dtls->chlo_total >> 8);
    p[16] = (uint8_t)(dtls->chlo_total & 0xFF);
    p[17] = (uint8_t)(dtls->chlo_msg_seq >> 8);
    p[18] = (uint8_t)(dtls->chlo_msg_seq & 0xFF);
    p[19] = p[20] = p[21] = 0;                 /* fragment_offset = 0 */
    p[22] = (uint8_t)(dtls->chlo_total >> 16); /* fragment_length = total */
    p[23] = (uint8_t)(dtls->chlo_total >> 8);
    p[24] = (uint8_t)(dtls->chlo_total & 0xFF);
    memcpy(&p[25], dtls->app_buf, dtls->chlo_total);
    dtls->in_len += new_rec_len;

    dtls->chlo_total = 0;
    dtls->chlo_have = 0;
    return 1;
}

int dtls_handle_data(nano_dtls_t *dtls, const uint8_t *data, size_t len)
{
    if (!dtls || !data || len == 0) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (!dtls->crypto || !dtls->crypto_ctx) {
        return NANORTC_ERR_STATE;
    }

    /* ClientHello fragment reassembly (server-side only). */
    int rasm = dtls_try_reassemble_chlo(dtls, data, len);
    if (rasm < 0) {
        return NANORTC_ERR_PROTOCOL;
    }
    if (rasm == 0) {
        /* Not a ClientHello fragment — normal pass-through buffering. */
        if (dtls->in_len + len > NANORTC_DTLS_BUF_SIZE) {
            return NANORTC_ERR_BUFFER_TOO_SMALL;
        }
        memcpy(dtls->in_buf + dtls->in_len, data, len);
        dtls->in_len += len;
    }
    /* rasm == 1: fragment buffered or full ClientHello emitted into in_buf.
     * If only partial data is in_buf, the handshake call below returns
     * WANT_READ and we wait for the next fragment. */

    nanortc_crypto_dtls_ctx_t *ctx = (nanortc_crypto_dtls_ctx_t *)dtls->crypto_ctx;

    if (dtls->state == NANORTC_DTLS_STATE_INIT || dtls->state == NANORTC_DTLS_STATE_HANDSHAKING) {
        /* Handshake phase: drive the handshake forward */
        if (dtls->state == NANORTC_DTLS_STATE_INIT) {
            dtls->state = NANORTC_DTLS_STATE_HANDSHAKING;
        }

        int rc = dtls->crypto->dtls_handshake(ctx);
        if (rc < 0) {
            NANORTC_LOGW("DTLS", "handshake rejected by crypto backend");
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

void dtls_close(nano_dtls_t *dtls)
{
    if (!dtls) {
        return;
    }
    /* Send DTLS close_notify alert (RFC 6347 §4.1.2.1).
     * The close_notify record is written via the BIO send callback,
     * which queues it in the DTLS output buffer for poll_output. */
    if (dtls->crypto && dtls->crypto->dtls_close_notify && dtls->crypto_ctx &&
        dtls->state == NANORTC_DTLS_STATE_ESTABLISHED) {
        dtls->crypto->dtls_close_notify((nanortc_crypto_dtls_ctx_t *)dtls->crypto_ctx);
    }
    dtls->state = NANORTC_DTLS_STATE_CLOSED;
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
