/*
 * nanortc — mbedtls crypto provider
 *
 * For embedded targets (ESP32, etc.) and CI testing.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_crypto.h"
#include <mbedtls/md.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_cookie.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/ecp.h>
#include <mbedtls/sha256.h>
#include <mbedtls/timing.h>
#include <mbedtls/error.h>
#include <mbedtls/bignum.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- HMAC-SHA1 (for STUN MESSAGE-INTEGRITY, RFC 8489 §14.5) ---- */

static void mbed_hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len,
                           uint8_t out[20])
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    mbedtls_md_hmac(info, key, key_len, data, data_len, out);
}

/* ---- CSPRNG (lazy-init static context) ---- */

static mbedtls_entropy_context mbed_entropy;
static mbedtls_ctr_drbg_context mbed_ctr_drbg;
static int mbed_rng_initialized = 0;

static int mbed_rng_init(void)
{
    if (mbed_rng_initialized) {
        return 0;
    }
    mbedtls_entropy_init(&mbed_entropy);
    mbedtls_ctr_drbg_init(&mbed_ctr_drbg);
    const char *pers = "nanortc";
    int ret = mbedtls_ctr_drbg_seed(&mbed_ctr_drbg, mbedtls_entropy_func, &mbed_entropy,
                                    (const unsigned char *)pers, 7);
    if (ret != 0) {
        return -1;
    }
    mbed_rng_initialized = 1;
    return 0;
}

static int mbed_random_bytes(uint8_t *buf, size_t len)
{
    if (mbed_rng_init() != 0) {
        return -1;
    }
    if (mbedtls_ctr_drbg_random(&mbed_ctr_drbg, buf, len) != 0) {
        return -1;
    }
    return 0;
}

/* ---- DTLS context (heap-allocated by provider) ---- */

struct nano_crypto_dtls_ctx {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt cert;
    mbedtls_pk_context pkey;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_timing_delay_context timer;
    mbedtls_ssl_cookie_ctx cookie_ctx;
    int is_server;
    int handshake_done;

    /* BIO callbacks (point back to nano_dtls_t buffers) */
    void *bio_userdata;
    nano_dtls_send_fn bio_send;
    nano_dtls_recv_fn bio_recv;

    /* Key export state (captured during handshake via ext callback) */
    uint8_t master_secret[48];
    uint8_t randbytes[64];
    mbedtls_tls_prf_types tls_prf_type;
    int keys_captured;

    /* Fingerprint cache */
    char fingerprint[97]; /* "XX:XX:..." SHA-256, 95 chars + NUL */
};

/* ---- Certificate verification callback: accept self-signed ---- */

static int mbed_verify_cb(void *data, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    (void)data;
    (void)crt;
    (void)depth;
    /* Clear flags that would reject self-signed certs */
    *flags &= ~(MBEDTLS_X509_BADCERT_NOT_TRUSTED | MBEDTLS_X509_BADCERT_CN_MISMATCH |
                MBEDTLS_X509_BADCERT_BAD_KEY);
    return 0;
}

/* ---- Key export callback (mbedtls 3.x API) ---- */

static void mbed_key_export_cb(void *p_expkey, mbedtls_ssl_key_export_type type,
                               const unsigned char *secret, size_t secret_len,
                               const unsigned char client_random[32],
                               const unsigned char server_random[32],
                               mbedtls_tls_prf_types tls_prf_type)
{
    nano_crypto_dtls_ctx_t *ctx = (nano_crypto_dtls_ctx_t *)p_expkey;
    /* Only capture TLS 1.2 master secret */
    if (type != MBEDTLS_SSL_KEY_EXPORT_TLS12_MASTER_SECRET) {
        return;
    }
    size_t copy_len = secret_len < 48 ? secret_len : 48;
    memcpy(ctx->master_secret, secret, copy_len);
    memcpy(ctx->randbytes, client_random, 32);
    memcpy(ctx->randbytes + 32, server_random, 32);
    ctx->tls_prf_type = tls_prf_type;
    ctx->keys_captured = 1;
}

/* ---- BIO wrappers (translate 0 → MBEDTLS_ERR_SSL_WANT_READ) ---- */

static int mbed_bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    nano_crypto_dtls_ctx_t *dtls_ctx = (nano_crypto_dtls_ctx_t *)ctx;
    if (!dtls_ctx->bio_send) {
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    int ret = dtls_ctx->bio_send(dtls_ctx->bio_userdata, buf, len);
    if (ret <= 0) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    return ret;
}

static int mbed_bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    nano_crypto_dtls_ctx_t *dtls_ctx = (nano_crypto_dtls_ctx_t *)ctx;
    if (!dtls_ctx->bio_recv) {
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    int ret = dtls_ctx->bio_recv(dtls_ctx->bio_userdata, buf, len);
    if (ret == 0) {
        return MBEDTLS_ERR_SSL_WANT_READ; /* No data available — Sans I/O signal */
    }
    if (ret < 0) {
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    return ret;
}

/* ---- Compute SHA-256 fingerprint of DER-encoded certificate ---- */

static int mbed_compute_fingerprint(const mbedtls_x509_crt *crt, char *buf, size_t buf_len)
{
    if (buf_len < 96) {
        return -1;
    }
    unsigned char digest[32];
    mbedtls_sha256_context sha256;
    mbedtls_sha256_init(&sha256);
    mbedtls_sha256_starts(&sha256, 0); /* 0 = SHA-256, not SHA-224 */
    mbedtls_sha256_update(&sha256, crt->raw.p, crt->raw.len);
    mbedtls_sha256_finish(&sha256, digest);
    mbedtls_sha256_free(&sha256);

    /* Format as "XX:XX:XX:..." (95 chars for SHA-256) */
    for (int i = 0; i < 32; i++) {
        snprintf(buf + i * 3, 4, "%02X:", digest[i]);
    }
    buf[95] = '\0'; /* Replace trailing ':' with NUL */
    return 0;
}

/* ---- Self-signed ECDSA P-256 certificate generation ---- */

static int mbed_generate_cert(nano_crypto_dtls_ctx_t *ctx)
{
    int ret;

    /* Generate ECDSA P-256 private key */
    mbedtls_pk_init(&ctx->pkey);
    ret = mbedtls_pk_setup(&ctx->pkey, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (ret != 0) {
        return ret;
    }
    ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(ctx->pkey),
                              mbedtls_ctr_drbg_random, &ctx->ctr_drbg);
    if (ret != 0) {
        return ret;
    }

    /* Build self-signed X.509 v3 certificate */
    mbedtls_x509write_cert crt;
    mbedtls_x509write_crt_init(&crt);
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &ctx->pkey);
    mbedtls_x509write_crt_set_issuer_key(&crt, &ctx->pkey);

    ret = mbedtls_x509write_crt_set_subject_name(&crt, "CN=nanortc");
    if (ret != 0) {
        mbedtls_x509write_crt_free(&crt);
        return ret;
    }
    ret = mbedtls_x509write_crt_set_issuer_name(&crt, "CN=nanortc");
    if (ret != 0) {
        mbedtls_x509write_crt_free(&crt);
        return ret;
    }

    /* Validity period */
    ret = mbedtls_x509write_crt_set_validity(&crt, "20240101000000", "20340101000000");
    if (ret != 0) {
        mbedtls_x509write_crt_free(&crt);
        return ret;
    }

    /* Random serial number */
    mbedtls_mpi serial;
    mbedtls_mpi_init(&serial);
    ret = mbedtls_mpi_fill_random(&serial, 16, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);
    if (ret != 0) {
        mbedtls_mpi_free(&serial);
        mbedtls_x509write_crt_free(&crt);
        return ret;
    }
    /* Ensure serial is positive */
    ret = mbedtls_mpi_set_bit(&serial, 127, 0);
    if (ret != 0) {
        mbedtls_mpi_free(&serial);
        mbedtls_x509write_crt_free(&crt);
        return ret;
    }
    ret = mbedtls_x509write_crt_set_serial(&crt, &serial);
    mbedtls_mpi_free(&serial);
    if (ret != 0) {
        mbedtls_x509write_crt_free(&crt);
        return ret;
    }

    /* Write to PEM */
    unsigned char cert_buf[1024];
    ret = mbedtls_x509write_crt_pem(&crt, cert_buf, sizeof(cert_buf), mbedtls_ctr_drbg_random,
                                    &ctx->ctr_drbg);
    mbedtls_x509write_crt_free(&crt);
    if (ret != 0) {
        return ret;
    }

    /* Parse the PEM back into x509_crt */
    mbedtls_x509_crt_init(&ctx->cert);
    ret = mbedtls_x509_crt_parse(&ctx->cert, cert_buf, strlen((char *)cert_buf) + 1);
    if (ret != 0) {
        return ret;
    }

    /* Compute fingerprint */
    ret = mbed_compute_fingerprint(&ctx->cert, ctx->fingerprint, sizeof(ctx->fingerprint));
    return ret;
}

/* ---- DTLS provider functions ---- */

/* Allocate and initialize DTLS context — returns heap-allocated ctx */
static nano_crypto_dtls_ctx_t *mbed_dtls_ctx_new(int is_server)
{
    int ret;

    nano_crypto_dtls_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }
    ctx->is_server = is_server;

    /* Initialize RNG */
    mbedtls_entropy_init(&ctx->entropy);
    mbedtls_ctr_drbg_init(&ctx->ctr_drbg);
    const char *pers = "nanortc-dtls";
    ret = mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, mbedtls_entropy_func, &ctx->entropy,
                                (const unsigned char *)pers, strlen(pers));
    if (ret != 0) {
        goto fail;
    }

    /* Generate self-signed ECDSA certificate */
    ret = mbed_generate_cert(ctx);
    if (ret != 0) {
        goto fail;
    }

    /* Configure DTLS */
    mbedtls_ssl_config_init(&ctx->conf);
    ret = mbedtls_ssl_config_defaults(&ctx->conf,
                                      is_server ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_DATAGRAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        goto fail;
    }

    mbedtls_ssl_conf_rng(&ctx->conf, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);

    /* Accept self-signed certificates */
    mbedtls_ssl_conf_authmode(&ctx->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_verify(&ctx->conf, mbed_verify_cb, NULL);

    /* Set own certificate */
    ret = mbedtls_ssl_conf_own_cert(&ctx->conf, &ctx->cert, &ctx->pkey);
    if (ret != 0) {
        goto fail;
    }

    /* Set CA chain to own cert (self-signed) */
    mbedtls_ssl_conf_ca_chain(&ctx->conf, &ctx->cert, NULL);

    /* Disable DTLS cookies — WebRTC uses ICE for peer verification.
     * Cookies add complexity (transport ID, HelloVerifyRequest) without
     * security benefit when ICE has already validated connectivity. */
    mbedtls_ssl_conf_dtls_cookies(&ctx->conf, NULL, NULL, NULL);

    /* Create SSL context */
    mbedtls_ssl_init(&ctx->ssl);
    ret = mbedtls_ssl_setup(&ctx->ssl, &ctx->conf);
    if (ret != 0) {
        goto fail;
    }

    /* Set timer callbacks for DTLS retransmission */
    mbedtls_ssl_set_timer_cb(&ctx->ssl, &ctx->timer, mbedtls_timing_set_delay,
                             mbedtls_timing_get_delay);

    /* Register key export callback (mbedtls 3.x: on ssl context) */
    mbedtls_ssl_set_export_keys_cb(&ctx->ssl, mbed_key_export_cb, ctx);

    return ctx;

fail:
    /* Cleanup on failure */
    mbedtls_ssl_free(&ctx->ssl);
    mbedtls_ssl_config_free(&ctx->conf);
    mbedtls_x509_crt_free(&ctx->cert);
    mbedtls_pk_free(&ctx->pkey);
    mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
    mbedtls_entropy_free(&ctx->entropy);
    /* cookie_ctx not used — cookies disabled for WebRTC */
    free(ctx);
    return NULL;
}

static int mbed_dtls_set_bio(nano_crypto_dtls_ctx_t *ctx, void *userdata, nano_dtls_send_fn send_cb,
                             nano_dtls_recv_fn recv_cb)
{
    if (!ctx) {
        return -1;
    }
    ctx->bio_userdata = userdata;
    ctx->bio_send = send_cb;
    ctx->bio_recv = recv_cb;
    mbedtls_ssl_set_bio(&ctx->ssl, ctx, mbed_bio_send, mbed_bio_recv, NULL);
    return 0;
}

static int mbed_dtls_handshake(nano_crypto_dtls_ctx_t *ctx)
{
    if (!ctx) {
        return -1;
    }

    int ret = mbedtls_ssl_handshake(&ctx->ssl);

    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return 1; /* Need more data */
    }

    /* Note: HELLO_VERIFY_REQUIRED won't occur — cookies disabled for WebRTC */

    if (ret != 0) {
        return -1; /* Error */
    }

    ctx->handshake_done = 1;
    return 0; /* Handshake complete */
}

static int mbed_dtls_encrypt(nano_crypto_dtls_ctx_t *ctx, const uint8_t *in, size_t in_len,
                             uint8_t *out, size_t *out_len)
{
    (void)out;
    (void)out_len;
    if (!ctx || !ctx->handshake_done) {
        return -1;
    }
    /* mbedtls_ssl_write sends through BIO callback → out_buf */
    int ret = mbedtls_ssl_write(&ctx->ssl, in, in_len);
    if (ret < 0) {
        if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            return 1; /* Buffer full, try again */
        }
        return -1;
    }
    return 0;
}

static int mbed_dtls_decrypt(nano_crypto_dtls_ctx_t *ctx, const uint8_t *in, size_t in_len,
                             uint8_t *out, size_t *out_len)
{
    (void)in;
    (void)in_len;
    if (!ctx || !ctx->handshake_done) {
        return -1;
    }
    /* mbedtls_ssl_read reads from BIO callback (in_buf already has data) */
    int ret = mbedtls_ssl_read(&ctx->ssl, out, *out_len);
    if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
        *out_len = 0;
        return 1; /* No data available yet */
    }
    if (ret < 0) {
        *out_len = 0;
        return -1;
    }
    *out_len = (size_t)ret;
    return 0;
}

static int mbed_dtls_export_keying_material(nano_crypto_dtls_ctx_t *ctx, const char *label,
                                            uint8_t *out, size_t out_len)
{
    if (!ctx || !ctx->keys_captured) {
        return -1;
    }
    /* RFC 5764: derive keying material using TLS PRF */
    int ret = mbedtls_ssl_tls_prf(ctx->tls_prf_type, ctx->master_secret, 48, label, ctx->randbytes,
                                  64, out, out_len);
    if (ret != 0) {
        return -1;
    }
    return 0;
}

static int mbed_dtls_get_fingerprint(nano_crypto_dtls_ctx_t *ctx, char *buf, size_t buf_len)
{
    if (!ctx || buf_len < 96) {
        return -1;
    }
    memcpy(buf, ctx->fingerprint, 96);
    return 0;
}

static void mbed_dtls_free(nano_crypto_dtls_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    mbedtls_ssl_free(&ctx->ssl);
    mbedtls_ssl_config_free(&ctx->conf);
    mbedtls_x509_crt_free(&ctx->cert);
    mbedtls_pk_free(&ctx->pkey);
    mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
    mbedtls_entropy_free(&ctx->entropy);
    if (ctx->is_server) {
        mbedtls_ssl_cookie_free(&ctx->cookie_ctx);
    }
    free(ctx);
}

#if NANORTC_PROFILE >= NANO_PROFILE_AUDIO
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

static const nano_crypto_provider_t mbedtls_provider = {
    .name = "mbedtls",
    .dtls_ctx_new = mbed_dtls_ctx_new,
    .dtls_set_bio = mbed_dtls_set_bio,
    .dtls_handshake = mbed_dtls_handshake,
    .dtls_encrypt = mbed_dtls_encrypt,
    .dtls_decrypt = mbed_dtls_decrypt,
    .dtls_export_keying_material = mbed_dtls_export_keying_material,
    .dtls_get_fingerprint = mbed_dtls_get_fingerprint,
    .dtls_free = mbed_dtls_free,
    .hmac_sha1 = mbed_hmac_sha1,
    .random_bytes = mbed_random_bytes,
#if NANORTC_PROFILE >= NANO_PROFILE_AUDIO
    .aes_128_cm = stub_aes_128_cm,
    .hmac_sha1_80 = stub_hmac_sha1_80,
#endif
};

const nano_crypto_provider_t *nano_crypto_mbedtls(void)
{
    return &mbedtls_provider;
}
