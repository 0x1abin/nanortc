/*
 * nanortc — OpenSSL crypto provider
 *
 * For Linux/desktop host builds and CI testing.
 * OpenSSL is widely available on Linux and macOS.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_crypto.h"
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/ec.h>
#include <stdlib.h>
#include <string.h>

/* ---- HMAC-SHA1 (for STUN MESSAGE-INTEGRITY, RFC 8489 §14.5) ---- */

static void ossl_hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len,
                           uint8_t out[20])
{
    unsigned int md_len = 20;
    HMAC(EVP_sha1(), key, (int)key_len, data, data_len, out, &md_len);
}

/* ---- CSPRNG ---- */

static int ossl_random_bytes(uint8_t *buf, size_t len)
{
    if (RAND_bytes(buf, (int)len) != 1) {
        return -1;
    }
    return 0;
}

/* ---- DTLS context (heap-allocated by provider) ---- */

struct nano_crypto_dtls_ctx {
    SSL_CTX *ssl_ctx;
    SSL *ssl;
    BIO *bio_in;  /* memory BIO: we write incoming, OpenSSL reads */
    BIO *bio_out; /* memory BIO: OpenSSL writes, we read outgoing */
    X509 *cert;
    EVP_PKEY *pkey;
    int is_server;
    int handshake_done;

    /* BIO adapter callbacks (from nano_dtls.c) */
    void *bio_userdata;
    nano_dtls_send_fn bio_send;
    nano_dtls_recv_fn bio_recv;

    /* Fingerprint cache */
    char fingerprint[NANO_DTLS_FINGERPRINT_STR_SIZE]; /* "XX:XX:..." SHA-256, 95 chars + NUL */
};

/* ---- Certificate verification callback: accept self-signed ---- */

static int ossl_verify_cb(int preverify_ok, X509_STORE_CTX *ctx)
{
    (void)preverify_ok;
    (void)ctx;
    return 1; /* Accept all certificates (self-signed OK for WebRTC) */
}

/* ---- Compute SHA-256 fingerprint of certificate ---- */

static int ossl_compute_fingerprint(X509 *cert, char *buf, size_t buf_len)
{
    if (buf_len < NANO_DTLS_FINGERPRINT_MIN_BUF) {
        return -1;
    }
    unsigned char digest[32];
    unsigned int digest_len = 0;
    if (X509_digest(cert, EVP_sha256(), digest, &digest_len) != 1 || digest_len != 32) {
        return -1;
    }
    static const char hex_upper[] = "0123456789ABCDEF";
    for (unsigned int i = 0; i < 32; i++) {
        buf[i * 3] = hex_upper[(digest[i] >> 4) & 0xF];
        buf[i * 3 + 1] = hex_upper[digest[i] & 0xF];
        buf[i * 3 + 2] = ':';
    }
    buf[95] = '\0'; /* Replace trailing ':' with NUL */
    return 0;
}

/* ---- Self-signed ECDSA P-256 certificate generation ---- */

static int ossl_generate_cert(nano_crypto_dtls_ctx_t *ctx)
{
    /* Generate ECDSA P-256 key */
    ctx->pkey = EVP_EC_gen("P-256");
    if (!ctx->pkey) {
        return -1;
    }

    /* Create self-signed X.509 v3 certificate */
    ctx->cert = X509_new();
    if (!ctx->cert) {
        return -1;
    }

    X509_set_version(ctx->cert, 2); /* v3 */

    /* Random serial number */
    ASN1_INTEGER *serial = ASN1_INTEGER_new();
    if (serial) {
        unsigned char serial_bytes[16];
        RAND_bytes(serial_bytes, sizeof(serial_bytes));
        serial_bytes[0] &= 0x7F; /* Ensure positive */
        BIGNUM *bn = BN_bin2bn(serial_bytes, sizeof(serial_bytes), NULL);
        if (bn) {
            BN_to_ASN1_INTEGER(bn, serial);
            X509_set_serialNumber(ctx->cert, serial);
            BN_free(bn);
        }
        ASN1_INTEGER_free(serial);
    }

    /* Validity: now to +10 years */
    X509_gmtime_adj(X509_get_notBefore(ctx->cert), 0);
    X509_gmtime_adj(X509_get_notAfter(ctx->cert), (long)365 * 24 * 3600 * 10);

    /* Subject and issuer (self-signed) */
    X509_NAME *name = X509_get_subject_name(ctx->cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)"nanortc", -1, -1,
                               0);
    X509_set_issuer_name(ctx->cert, name);

    /* Set public key and sign */
    X509_set_pubkey(ctx->cert, ctx->pkey);
    if (X509_sign(ctx->cert, ctx->pkey, EVP_sha256()) <= 0) {
        return -1;
    }

    /* Compute fingerprint */
    return ossl_compute_fingerprint(ctx->cert, ctx->fingerprint, sizeof(ctx->fingerprint));
}

/* ---- Drain outgoing data from bio_out and send via BIO callback ---- */

static int ossl_drain_bio_out(nano_crypto_dtls_ctx_t *ctx)
{
    if (!ctx->bio_send) {
        return 0;
    }
    char buf[2048];
    int pending;
    while ((pending = (int)BIO_ctrl_pending(ctx->bio_out)) > 0) {
        int n = BIO_read(ctx->bio_out, buf,
                         (int)(pending < (int)sizeof(buf) ? pending : (int)sizeof(buf)));
        if (n <= 0) {
            break;
        }
        int sent = ctx->bio_send(ctx->bio_userdata, (const uint8_t *)buf, (size_t)n);
        if (sent <= 0) {
            return -1;
        }
    }
    return 0;
}

/* ---- Feed incoming data from BIO callback into bio_in ---- */

static int ossl_feed_bio_in(nano_crypto_dtls_ctx_t *ctx)
{
    if (!ctx->bio_recv) {
        return 0;
    }
    uint8_t buf[2048];
    int n = ctx->bio_recv(ctx->bio_userdata, buf, sizeof(buf));
    if (n > 0) {
        BIO_write(ctx->bio_in, buf, n);
    }
    return n;
}

/* ---- DTLS provider functions ---- */

static nano_crypto_dtls_ctx_t *ossl_dtls_ctx_new(int is_server)
{
    nano_crypto_dtls_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }
    ctx->is_server = is_server;

    /* Generate self-signed certificate */
    if (ossl_generate_cert(ctx) != 0) {
        goto fail;
    }

    /* Create SSL_CTX for DTLS 1.2 */
    ctx->ssl_ctx = SSL_CTX_new(DTLS_method());
    if (!ctx->ssl_ctx) {
        goto fail;
    }

    /* Force DTLS 1.2 minimum */
    SSL_CTX_set_min_proto_version(ctx->ssl_ctx, DTLS1_2_VERSION);

    /* Set own certificate and key */
    if (SSL_CTX_use_certificate(ctx->ssl_ctx, ctx->cert) != 1) {
        goto fail;
    }
    if (SSL_CTX_use_PrivateKey(ctx->ssl_ctx, ctx->pkey) != 1) {
        goto fail;
    }

    /* Accept self-signed peer certificates */
    SSL_CTX_set_verify(ctx->ssl_ctx, SSL_VERIFY_PEER, ossl_verify_cb);

    /* Configure cipher suites */
    SSL_CTX_set_cipher_list(ctx->ssl_ctx,
                            "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384");

    /* Configure DTLS-SRTP profiles */
    SSL_CTX_set_tlsext_use_srtp(ctx->ssl_ctx, "SRTP_AES128_CM_SHA1_80:SRTP_AES128_CM_SHA1_32");

    /* Create SSL instance */
    ctx->ssl = SSL_new(ctx->ssl_ctx);
    if (!ctx->ssl) {
        goto fail;
    }

    /* Create memory BIO pair */
    ctx->bio_in = BIO_new(BIO_s_mem());
    ctx->bio_out = BIO_new(BIO_s_mem());
    if (!ctx->bio_in || !ctx->bio_out) {
        goto fail;
    }

    /* Make bio_in non-blocking */
    BIO_set_mem_eof_return(ctx->bio_in, -1);

    /* SSL_set_bio takes ownership of BIOs */
    SSL_set_bio(ctx->ssl, ctx->bio_in, ctx->bio_out);
    /* After SSL_set_bio, don't free bio_in/bio_out separately — SSL_free does it */

    /* Set MTU for DTLS */
    SSL_set_options(ctx->ssl, SSL_OP_NO_QUERY_MTU);
    DTLS_set_link_mtu(ctx->ssl, 1200);

    /* Set role — no cookie exchange (ICE already verifies peers) */
    if (is_server) {
        SSL_set_accept_state(ctx->ssl);
    } else {
        SSL_set_connect_state(ctx->ssl);
    }

    return ctx;

fail:
    if (ctx->ssl) {
        SSL_free(ctx->ssl); /* Also frees BIOs */
        ctx->bio_in = NULL;
        ctx->bio_out = NULL;
    } else {
        if (ctx->bio_in) {
            BIO_free(ctx->bio_in);
        }
        if (ctx->bio_out) {
            BIO_free(ctx->bio_out);
        }
    }
    if (ctx->ssl_ctx) {
        SSL_CTX_free(ctx->ssl_ctx);
    }
    if (ctx->cert) {
        X509_free(ctx->cert);
    }
    if (ctx->pkey) {
        EVP_PKEY_free(ctx->pkey);
    }
    free(ctx);
    return NULL;
}

static int ossl_dtls_set_bio(nano_crypto_dtls_ctx_t *ctx, void *userdata, nano_dtls_send_fn send_cb,
                             nano_dtls_recv_fn recv_cb)
{
    if (!ctx) {
        return -1;
    }
    ctx->bio_userdata = userdata;
    ctx->bio_send = send_cb;
    ctx->bio_recv = recv_cb;
    return 0;
}

static int ossl_dtls_handshake(nano_crypto_dtls_ctx_t *ctx)
{
    if (!ctx) {
        return -1;
    }

    /* Feed incoming data from the BIO callback into OpenSSL's read BIO */
    ossl_feed_bio_in(ctx);

    /* Drive the handshake */
    int ret = SSL_do_handshake(ctx->ssl);

    /* Drain any outgoing data from OpenSSL's write BIO */
    ossl_drain_bio_out(ctx);

    if (ret == 1) {
        ctx->handshake_done = 1;
        return 0; /* Handshake complete */
    }

    int err = SSL_get_error(ctx->ssl, ret);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        return 1; /* Need more data */
    }

    return -1; /* Error */
}

static int ossl_dtls_encrypt(nano_crypto_dtls_ctx_t *ctx, const uint8_t *in, size_t in_len,
                             uint8_t *out, size_t *out_len)
{
    (void)out;
    (void)out_len;
    if (!ctx || !ctx->handshake_done) {
        return -1;
    }

    int ret = SSL_write(ctx->ssl, in, (int)in_len);
    if (ret <= 0) {
        return -1;
    }

    /* Drain the encrypted record from bio_out via send callback */
    ossl_drain_bio_out(ctx);
    return 0;
}

static int ossl_dtls_decrypt(nano_crypto_dtls_ctx_t *ctx, const uint8_t *in, size_t in_len,
                             uint8_t *out, size_t *out_len)
{
    (void)in;
    (void)in_len;
    if (!ctx || !ctx->handshake_done) {
        return -1;
    }

    /* Feed data via BIO callback into bio_in */
    ossl_feed_bio_in(ctx);

    int ret = SSL_read(ctx->ssl, out, (int)*out_len);
    if (ret <= 0) {
        int err = SSL_get_error(ctx->ssl, ret);
        if (err == SSL_ERROR_WANT_READ) {
            *out_len = 0;
            return 1; /* No data available yet */
        }
        *out_len = 0;
        return -1;
    }
    *out_len = (size_t)ret;
    return 0;
}

static int ossl_dtls_export_keying_material(nano_crypto_dtls_ctx_t *ctx, const char *label,
                                            size_t label_len, uint8_t *out, size_t out_len)
{
    if (!ctx || !ctx->handshake_done) {
        return -1;
    }
    /* RFC 5764: export keying material with DTLS-SRTP label */
    if (SSL_export_keying_material(ctx->ssl, out, out_len, label, label_len, NULL, 0, 0) != 1) {
        return -1;
    }
    return 0;
}

static int ossl_dtls_get_fingerprint(nano_crypto_dtls_ctx_t *ctx, char *buf, size_t buf_len)
{
    if (!ctx || buf_len < NANO_DTLS_FINGERPRINT_MIN_BUF) {
        return -1;
    }
    memcpy(buf, ctx->fingerprint, NANO_DTLS_FINGERPRINT_MIN_BUF);
    return 0;
}

static void ossl_dtls_free(nano_crypto_dtls_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    if (ctx->ssl) {
        SSL_free(ctx->ssl); /* Also frees the BIOs */
    } else {
        /* BIOs not yet attached to SSL */
        if (ctx->bio_in) {
            BIO_free(ctx->bio_in);
        }
        if (ctx->bio_out) {
            BIO_free(ctx->bio_out);
        }
    }
    if (ctx->ssl_ctx) {
        SSL_CTX_free(ctx->ssl_ctx);
    }
    if (ctx->cert) {
        X509_free(ctx->cert);
    }
    if (ctx->pkey) {
        EVP_PKEY_free(ctx->pkey);
    }
    free(ctx);
}

#if NANO_HAVE_MEDIA_TRANSPORT
static int stub_aes_128_cm(const uint8_t key[16], const uint8_t iv[16], const uint8_t *in,
                           size_t len, uint8_t *out)
{
    (void)key;
    (void)iv;
    (void)in;
    (void)len;
    (void)out;
    return -1;
}

static void stub_hmac_sha1_80(const uint8_t *key, size_t key_len, const uint8_t *data,
                              size_t data_len, uint8_t out[10])
{
    (void)key;
    (void)key_len;
    (void)data;
    (void)data_len;
    (void)out;
}
#endif

/* ---- Provider instance ---- */

static const nano_crypto_provider_t openssl_provider = {
    .name = "openssl",
    .dtls_ctx_new = ossl_dtls_ctx_new,
    .dtls_set_bio = ossl_dtls_set_bio,
    .dtls_handshake = ossl_dtls_handshake,
    .dtls_encrypt = ossl_dtls_encrypt,
    .dtls_decrypt = ossl_dtls_decrypt,
    .dtls_export_keying_material = ossl_dtls_export_keying_material,
    .dtls_get_fingerprint = ossl_dtls_get_fingerprint,
    .dtls_free = ossl_dtls_free,
    .hmac_sha1 = ossl_hmac_sha1,
    .random_bytes = ossl_random_bytes,
#if NANO_HAVE_MEDIA_TRANSPORT
    .aes_128_cm = stub_aes_128_cm,
    .hmac_sha1_80 = stub_hmac_sha1_80,
#endif
};

const nano_crypto_provider_t *nano_crypto_openssl(void)
{
    return &openssl_provider;
}
