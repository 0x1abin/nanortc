/*
 * nanortc — mbedtls crypto provider
 *
 * Supports mbedtls 2.x, 3.x, and 4.x (PSA Crypto).
 * For embedded targets (ESP32, etc.) and CI testing.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc_crypto.h"
#include <mbedtls/version.h>

/* ---- mbedtls version compatibility ---- */

/* mbedtls 3.x vs 2.x: key export API and mbedtls_pk_ec() differ */
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
#define NANORTC_MBEDTLS_3
#endif

/* mbedtls 3.6+: mbedtls_pk_ec()/ecp_gen_key() may be unavailable
 * (ESP-IDF defines MBEDTLS_DEPRECATED_REMOVED);
 * x509write_crt_set_serial() replaced by _set_serial_raw();
 * PSA Crypto available for key generation via pk_copy_from_psa(). */
#if MBEDTLS_VERSION_NUMBER >= 0x03060000 && MBEDTLS_VERSION_NUMBER < 0x04000000
#define NANORTC_MBEDTLS_3_6
#endif

/* mbedtls 4.x: PSA Crypto API replaces legacy low-level APIs.
 * entropy/ctr_drbg/ecp/bignum/sha256 headers removed;
 * pk_setup()/pk_info_from_type()/ecp_gen_key() removed;
 * md_hmac() moved behind MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS;
 * x509write_crt_set_serial() replaced by _set_serial_raw();
 * x509write_crt_pem()/der() no longer take RNG params;
 * ssl_conf_rng() removed (SSL uses PSA RNG internally). */
#if MBEDTLS_VERSION_NUMBER >= 0x04000000
#define NANORTC_MBEDTLS_4
#endif

#ifdef NANORTC_MBEDTLS_4
/* ---- mbedtls 4.x headers ---- */
#include <psa/crypto.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_cookie.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/md.h>
#include <mbedtls/timing.h>
#include <mbedtls/error.h>
#else
/* ---- mbedtls 2.x/3.x headers ---- */
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
#ifdef NANORTC_MBEDTLS_3_6
#include <psa/crypto.h>
#endif
#endif

#include "nanortc_util.h"
#include <stdlib.h>
#include <string.h>

/* ---- Version-adaptive macros (2.x/3.x only) ---- */

#if !defined(NANORTC_MBEDTLS_4) && !defined(NANORTC_MBEDTLS_3_6)
/* mbedtls < 3.6: legacy PK + ECP key generation */
#define NANORTC_PK_EC(pk) mbedtls_pk_ec(pk)
#endif

#ifndef NANORTC_MBEDTLS_4
/*
 * mbedtls 2.x deprecated the non-_ret SHA functions.
 * Use _ret variants on 2.x to avoid -Werror=deprecated-declarations.
 * In 3.x the _ret variants were removed and the plain names return int.
 */
#ifdef NANORTC_MBEDTLS_3
#define nano_sha256_starts mbedtls_sha256_starts
#define nano_sha256_update mbedtls_sha256_update
#define nano_sha256_finish mbedtls_sha256_finish
#else
#define nano_sha256_starts mbedtls_sha256_starts_ret
#define nano_sha256_update mbedtls_sha256_update_ret
#define nano_sha256_finish mbedtls_sha256_finish_ret
#endif
#endif /* !NANORTC_MBEDTLS_4 */

/* ================================================================
 * PSA Crypto initialization (mbedtls 4.x)
 * ================================================================ */

#if defined(NANORTC_MBEDTLS_4) || defined(NANORTC_MBEDTLS_3_6)
static int psa_initialized = 0;

static int mbed_psa_init(void)
{
    if (psa_initialized) {
        return 0;
    }
    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        return -1;
    }
    psa_initialized = 1;
    return 0;
}
#endif

/* ================================================================
 * HMAC-SHA1 (for STUN MESSAGE-INTEGRITY, RFC 8489 §14.5)
 * ================================================================ */

static void mbed_hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len,
                           uint8_t out[20])
{
#ifdef NANORTC_MBEDTLS_4
    /* PSA one-shot HMAC: import key → compute → destroy */
    if (mbed_psa_init() != 0) {
        nanortc_memzero(out, 20);
        return;
    }
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_1));
    psa_set_key_bits(&attr, (size_t)(key_len * 8));

    psa_key_id_t key_id = 0;
    psa_status_t status = psa_import_key(&attr, key, key_len, &key_id);
    if (status != PSA_SUCCESS) {
        nanortc_memzero(out, 20);
        return;
    }
    size_t mac_len = 0;
    psa_mac_compute(key_id, PSA_ALG_HMAC(PSA_ALG_SHA_1), data, data_len, out, 20, &mac_len);
    psa_destroy_key(key_id);
#else
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    mbedtls_md_hmac(info, key, key_len, data, data_len, out);
#endif
}

/* ================================================================
 * CSPRNG
 * ================================================================ */

#ifdef NANORTC_MBEDTLS_4
/* mbedtls 4.x: PSA provides psa_generate_random() directly */
static int mbed_random_bytes(uint8_t *buf, size_t len)
{
    if (mbed_psa_init() != 0) {
        return -1;
    }
    if (psa_generate_random(buf, len) != PSA_SUCCESS) {
        return -1;
    }
    return 0;
}
#else
/* mbedtls 2.x/3.x: entropy + CTR-DRBG */
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
#endif

/* ================================================================
 * DTLS context (heap-allocated by provider)
 * ================================================================ */

struct nanortc_crypto_dtls_ctx {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt cert;
    mbedtls_pk_context pkey;
#ifndef NANORTC_MBEDTLS_4
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
#endif
    mbedtls_timing_delay_context timer;
    mbedtls_ssl_cookie_ctx cookie_ctx;
    int is_server;
    int handshake_done;

    /* BIO callbacks (point back to nano_dtls_t buffers) */
    void *bio_userdata;
    nanortc_dtls_send_fn bio_send;
    nanortc_dtls_recv_fn bio_recv;

    /* Key export state (captured during handshake via ext callback) */
    uint8_t master_secret[48];
    uint8_t randbytes[64];
    mbedtls_tls_prf_types tls_prf_type;
    int keys_captured;

    /* Fingerprint cache */
    char fingerprint[NANORTC_DTLS_FINGERPRINT_STR_SIZE]; /* "XX:XX:..." SHA-256, 95 chars + NUL */
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

/* ---- Key export callback (version-adaptive: 3.x/4.x vs 2.x) ---- */

#ifdef NANORTC_MBEDTLS_3
/* mbedtls 3.x/4.x: callback on ssl context with key_export_type enum */
static void mbed_key_export_cb(void *p_expkey, mbedtls_ssl_key_export_type type,
                               const unsigned char *secret, size_t secret_len,
                               const unsigned char client_random[32],
                               const unsigned char server_random[32],
                               mbedtls_tls_prf_types tls_prf_type)
{
    nanortc_crypto_dtls_ctx_t *ctx = (nanortc_crypto_dtls_ctx_t *)p_expkey;
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
#else
/* mbedtls 2.x: callback on conf with extended key export */
static int mbed_key_export_cb(void *p_expkey, const unsigned char *ms, const unsigned char *kb,
                              size_t maclen, size_t keylen, size_t ivlen,
                              const unsigned char client_random[32],
                              const unsigned char server_random[32],
                              mbedtls_tls_prf_types tls_prf_type)
{
    (void)kb;
    (void)maclen;
    (void)keylen;
    (void)ivlen;
    nanortc_crypto_dtls_ctx_t *ctx = (nanortc_crypto_dtls_ctx_t *)p_expkey;
    memcpy(ctx->master_secret, ms, 48);
    memcpy(ctx->randbytes, client_random, 32);
    memcpy(ctx->randbytes + 32, server_random, 32);
    ctx->tls_prf_type = tls_prf_type;
    ctx->keys_captured = 1;
    return 0;
}
#endif

/* ---- BIO wrappers (translate 0 → MBEDTLS_ERR_SSL_WANT_READ) ---- */

static int mbed_bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    nanortc_crypto_dtls_ctx_t *dtls_ctx = (nanortc_crypto_dtls_ctx_t *)ctx;
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
    nanortc_crypto_dtls_ctx_t *dtls_ctx = (nanortc_crypto_dtls_ctx_t *)ctx;
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

/* ================================================================
 * Compute SHA-256 fingerprint of DER-encoded certificate
 * ================================================================ */

static int mbed_compute_fingerprint(const mbedtls_x509_crt *crt, char *buf, size_t buf_len)
{
    if (buf_len < NANORTC_DTLS_FINGERPRINT_MIN_BUF) {
        return -1;
    }
    unsigned char digest[32];

#ifdef NANORTC_MBEDTLS_4
    /* PSA one-shot hash (psa_crypto_init already called by mbed_dtls_ctx_new) */
    size_t hash_len = 0;
    psa_status_t status =
        psa_hash_compute(PSA_ALG_SHA_256, crt->raw.p, crt->raw.len, digest, 32, &hash_len);
    if (status != PSA_SUCCESS || hash_len != 32) {
        return -1;
    }
#else
    mbedtls_sha256_context sha256;
    mbedtls_sha256_init(&sha256);
    nano_sha256_starts(&sha256, 0); /* 0 = SHA-256, not SHA-224 */
    nano_sha256_update(&sha256, crt->raw.p, crt->raw.len);
    nano_sha256_finish(&sha256, digest);
    mbedtls_sha256_free(&sha256);
#endif

    /* Format as "XX:XX:XX:..." (95 chars for SHA-256) */
    static const char hex_upper[] = "0123456789ABCDEF";
    for (int i = 0; i < 32; i++) {
        buf[i * 3] = hex_upper[(digest[i] >> 4) & 0xF];
        buf[i * 3 + 1] = hex_upper[digest[i] & 0xF];
        buf[i * 3 + 2] = ':';
    }
    buf[95] = '\0'; /* Replace trailing ':' with NUL */
    return 0;
}

/* ================================================================
 * Self-signed ECDSA P-256 certificate generation
 * ================================================================ */

static int mbed_generate_cert(nanortc_crypto_dtls_ctx_t *ctx)
{
    int ret;

#if defined(NANORTC_MBEDTLS_4) || defined(NANORTC_MBEDTLS_3_6)
    /* mbedtls 3.6+/4.x: generate key via PSA, then copy into PK context.
     * Avoids mbedtls_pk_ec()/ecp_gen_key() which are removed when
     * MBEDTLS_DEPRECATED_REMOVED is defined (e.g. ESP-IDF). */
    if (mbed_psa_init() != 0) {
        return -1;
    }
    psa_key_attributes_t key_attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&key_attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&key_attr, 256);
    psa_set_key_usage_flags(&key_attr, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_SIGN_MESSAGE |
                                           PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&key_attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));

    psa_key_id_t psa_key_id = 0;
    psa_status_t status = psa_generate_key(&key_attr, &psa_key_id);
    if (status != PSA_SUCCESS) {
        return -1;
    }

    /* Copy PSA key into mbedtls PK context for x509write */
    mbedtls_pk_init(&ctx->pkey);
    ret = mbedtls_pk_copy_from_psa(psa_key_id, &ctx->pkey);
    psa_destroy_key(psa_key_id);
    if (ret != 0) {
        return ret;
    }
#else
    /* mbedtls 2.x/3.x (<3.6): legacy PK + ECP key generation */
    mbedtls_pk_init(&ctx->pkey);
    ret = mbedtls_pk_setup(&ctx->pkey, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (ret != 0) {
        return ret;
    }
    ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, NANORTC_PK_EC(ctx->pkey),
                              mbedtls_ctr_drbg_random, &ctx->ctr_drbg);
    if (ret != 0) {
        return ret;
    }
#endif

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
#if defined(NANORTC_MBEDTLS_4) || defined(NANORTC_MBEDTLS_3_6)
    /* mbedtls 3.6+/4.x: set_serial_raw() with byte buffer */
    {
        unsigned char serial_buf[16];
        if (psa_generate_random(serial_buf, sizeof(serial_buf)) != PSA_SUCCESS) {
            mbedtls_x509write_crt_free(&crt);
            return -1;
        }
        serial_buf[0] &= 0x7F; /* Ensure positive (clear MSB) */
        ret = mbedtls_x509write_crt_set_serial_raw(&crt, serial_buf, sizeof(serial_buf));
        if (ret != 0) {
            mbedtls_x509write_crt_free(&crt);
            return ret;
        }
    }
#else
    {
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
    }
#endif

    /* Write to PEM */
    unsigned char cert_buf[1024];
#ifdef NANORTC_MBEDTLS_4
    /* mbedtls 4.x: no RNG params (ssl uses PSA RNG internally) */
    ret = mbedtls_x509write_crt_pem(&crt, cert_buf, sizeof(cert_buf));
#else
    ret = mbedtls_x509write_crt_pem(&crt, cert_buf, sizeof(cert_buf), mbedtls_ctr_drbg_random,
                                    &ctx->ctr_drbg);
#endif
    mbedtls_x509write_crt_free(&crt);
    if (ret != 0) {
        return ret;
    }

    /* Parse the PEM back into x509_crt */
    mbedtls_x509_crt_init(&ctx->cert);
    ret = mbedtls_x509_crt_parse(&ctx->cert, cert_buf,
                                 nanortc_strnlen((const char *)cert_buf, sizeof(cert_buf)) + 1);
    if (ret != 0) {
        return ret;
    }

    /* Compute fingerprint */
    ret = mbed_compute_fingerprint(&ctx->cert, ctx->fingerprint, sizeof(ctx->fingerprint));
    return ret;
}

/* ================================================================
 * DTLS provider functions
 * ================================================================ */

/* Allocate and initialize DTLS context — returns heap-allocated ctx */
static nanortc_crypto_dtls_ctx_t *mbed_dtls_ctx_new(int is_server)
{
    int ret;

#if defined(NANORTC_MBEDTLS_4) || defined(NANORTC_MBEDTLS_3_6)
    /* Ensure PSA crypto is initialized */
    if (mbed_psa_init() != 0) {
        return NULL;
    }
#endif

    nanortc_crypto_dtls_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }
    ctx->is_server = is_server;

#ifndef NANORTC_MBEDTLS_4
    /* mbedtls 2.x/3.x: Initialize per-context RNG */
    mbedtls_entropy_init(&ctx->entropy);
    mbedtls_ctr_drbg_init(&ctx->ctr_drbg);
    const char *pers = "nanortc-dtls";
    ret = mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, mbedtls_entropy_func, &ctx->entropy,
                                (const unsigned char *)pers, sizeof("nanortc-dtls") - 1);
    if (ret != 0) {
        goto fail;
    }
#endif

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

#ifndef NANORTC_MBEDTLS_4
    /* mbedtls 2.x/3.x: explicit RNG config (4.x uses PSA RNG internally) */
    mbedtls_ssl_conf_rng(&ctx->conf, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);
#endif

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

    /* Register key export callback (mbedtls 2.x: on conf before ssl_setup) */
#ifndef NANORTC_MBEDTLS_3
    mbedtls_ssl_conf_export_keys_ext_cb(&ctx->conf, mbed_key_export_cb, ctx);
#endif

    /* Create SSL context */
    mbedtls_ssl_init(&ctx->ssl);
    ret = mbedtls_ssl_setup(&ctx->ssl, &ctx->conf);
    if (ret != 0) {
        goto fail;
    }

    /* Set timer callbacks for DTLS retransmission */
    mbedtls_ssl_set_timer_cb(&ctx->ssl, &ctx->timer, mbedtls_timing_set_delay,
                             mbedtls_timing_get_delay);

    /* Register key export callback (mbedtls 3.x/4.x: on ssl context) */
#ifdef NANORTC_MBEDTLS_3
    mbedtls_ssl_set_export_keys_cb(&ctx->ssl, mbed_key_export_cb, ctx);
#endif

    return ctx;

fail:
    /* Cleanup on failure */
    mbedtls_ssl_free(&ctx->ssl);
    mbedtls_ssl_config_free(&ctx->conf);
    mbedtls_x509_crt_free(&ctx->cert);
    mbedtls_pk_free(&ctx->pkey);
#ifndef NANORTC_MBEDTLS_4
    mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
    mbedtls_entropy_free(&ctx->entropy);
#endif
    /* cookie_ctx not used — cookies disabled for WebRTC */
    free(ctx);
    return NULL;
}

static int mbed_dtls_set_bio(nanortc_crypto_dtls_ctx_t *ctx, void *userdata,
                             nanortc_dtls_send_fn send_cb, nanortc_dtls_recv_fn recv_cb)
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

static int mbed_dtls_handshake(nanortc_crypto_dtls_ctx_t *ctx)
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

static int mbed_dtls_encrypt(nanortc_crypto_dtls_ctx_t *ctx, const uint8_t *in, size_t in_len,
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

static int mbed_dtls_decrypt(nanortc_crypto_dtls_ctx_t *ctx, const uint8_t *in, size_t in_len,
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

static int mbed_dtls_export_keying_material(nanortc_crypto_dtls_ctx_t *ctx, const char *label,
                                            size_t label_len, uint8_t *out, size_t out_len)
{
    (void)label_len; /* mbedtls_ssl_tls_prf handles label as C string internally */
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

static int mbed_dtls_get_fingerprint(nanortc_crypto_dtls_ctx_t *ctx, char *buf, size_t buf_len)
{
    if (!ctx || buf_len < NANORTC_DTLS_FINGERPRINT_MIN_BUF) {
        return -1;
    }
    memcpy(buf, ctx->fingerprint, NANORTC_DTLS_FINGERPRINT_MIN_BUF);
    return 0;
}

/* Switch DTLS role without regenerating certificate (for actpass negotiation) */
static int mbed_dtls_set_role(nanortc_crypto_dtls_ctx_t *ctx, int is_server)
{
    if (!ctx)
        return -1;
    ctx->is_server = is_server;
    mbedtls_ssl_conf_endpoint(&ctx->conf,
                              is_server ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT);
    /* Reset SSL state machine to pick up new endpoint from config.
     * Safe before handshake; preserves certificate and config. */
    mbedtls_ssl_session_reset(&ctx->ssl);
    return 0;
}

static void mbed_dtls_free(nanortc_crypto_dtls_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    mbedtls_ssl_free(&ctx->ssl);
    mbedtls_ssl_config_free(&ctx->conf);
    mbedtls_x509_crt_free(&ctx->cert);
    mbedtls_pk_free(&ctx->pkey);
#ifndef NANORTC_MBEDTLS_4
    mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
    mbedtls_entropy_free(&ctx->entropy);
#endif
    if (ctx->is_server) {
        mbedtls_ssl_cookie_free(&ctx->cookie_ctx);
    }
    free(ctx);
}

#if NANORTC_HAVE_MEDIA_TRANSPORT
/* AES-128 Counter Mode (RFC 3711 section 4.1.1) */
#ifdef NANORTC_MBEDTLS_4
static int mbed_aes_128_cm(const uint8_t key[16], const uint8_t iv[16], const uint8_t *in,
                           size_t len, uint8_t *out)
{
    if (mbed_psa_init() != 0) {
        return -1;
    }

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_CTR);
    psa_set_key_bits(&attr, 128);

    psa_key_id_t key_id = 0;
    psa_status_t status = psa_import_key(&attr, key, 16, &key_id);
    if (status != PSA_SUCCESS) {
        return -1;
    }

    /* PSA CTR: use multi-part cipher operation */
    psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
    status = psa_cipher_encrypt_setup(&op, key_id, PSA_ALG_CTR);
    if (status != PSA_SUCCESS) {
        psa_destroy_key(key_id);
        return -1;
    }

    status = psa_cipher_set_iv(&op, iv, 16);
    if (status != PSA_SUCCESS) {
        psa_cipher_abort(&op);
        psa_destroy_key(key_id);
        return -1;
    }

    size_t out_len = 0;
    status = psa_cipher_update(&op, in, len, out, len, &out_len);
    if (status != PSA_SUCCESS) {
        psa_cipher_abort(&op);
        psa_destroy_key(key_id);
        return -1;
    }

    size_t finish_len = 0;
    status = psa_cipher_finish(&op, out + out_len, len - out_len, &finish_len);
    psa_destroy_key(key_id);
    if (status != PSA_SUCCESS) {
        return -1;
    }

    return 0;
}
#else
#include <mbedtls/aes.h>

static int mbed_aes_128_cm(const uint8_t key[16], const uint8_t iv[16], const uint8_t *in,
                           size_t len, uint8_t *out)
{
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);

    if (mbedtls_aes_setkey_enc(&aes, key, 128) != 0) {
        mbedtls_aes_free(&aes);
        return -1;
    }

    uint8_t nonce_counter[16];
    uint8_t stream_block[16];
    size_t nc_off = 0;

    memcpy(nonce_counter, iv, 16);
    memset(stream_block, 0, 16);

    int ret = mbedtls_aes_crypt_ctr(&aes, len, &nc_off, nonce_counter, stream_block, in, out);
    mbedtls_aes_free(&aes);

    return ret == 0 ? 0 : -1;
}
#endif /* NANORTC_MBEDTLS_4 */

/* HMAC-SHA1 truncated to 80 bits (RFC 3711 section 4.2.1) */
static void mbed_hmac_sha1_80(const uint8_t *key, size_t key_len, const uint8_t *data,
                              size_t data_len, uint8_t out[10])
{
    uint8_t full[20];
    mbed_hmac_sha1(key, key_len, data, data_len, full);
    memcpy(out, full, 10);
}
#endif

/* ---- Provider instance ---- */

static const nanortc_crypto_provider_t mbedtls_provider = {
    .name = "mbedtls",
    .dtls_ctx_new = mbed_dtls_ctx_new,
    .dtls_set_bio = mbed_dtls_set_bio,
    .dtls_handshake = mbed_dtls_handshake,
    .dtls_encrypt = mbed_dtls_encrypt,
    .dtls_decrypt = mbed_dtls_decrypt,
    .dtls_export_keying_material = mbed_dtls_export_keying_material,
    .dtls_get_fingerprint = mbed_dtls_get_fingerprint,
    .dtls_free = mbed_dtls_free,
    .dtls_set_role = mbed_dtls_set_role,
    .hmac_sha1 = mbed_hmac_sha1,
    .random_bytes = mbed_random_bytes,
#if NANORTC_HAVE_MEDIA_TRANSPORT
    .aes_128_cm = mbed_aes_128_cm,
    .hmac_sha1_80 = mbed_hmac_sha1_80,
#endif
};

const nanortc_crypto_provider_t *nanortc_crypto_mbedtls(void)
{
    return &mbedtls_provider;
}
