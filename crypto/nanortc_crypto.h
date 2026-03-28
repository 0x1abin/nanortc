/*
 * nanortc — Pluggable crypto provider interface
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_CRYPTO_H_
#define NANORTC_CRYPTO_H_

#include "nanortc_config.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef NANORTC_API
#if defined(__GNUC__) || defined(__clang__)
#define NANORTC_API __attribute__((visibility("default")))
#else
#define NANORTC_API
#endif
#endif

/* DTLS certificate fingerprint format: "XX:XX:...:XX" SHA-256 */
#define NANORTC_DTLS_FINGERPRINT_STR_SIZE 97 /* 95 hex:colon chars + NUL + 1 spare */
#define NANORTC_DTLS_FINGERPRINT_MIN_BUF  96 /* 95 chars + NUL */

/* Opaque DTLS context — sized by the provider implementation */
typedef struct nanortc_crypto_dtls_ctx nanortc_crypto_dtls_ctx_t;

/* BIO callbacks for Sans I/O DTLS */
typedef int (*nanortc_dtls_send_fn)(void *userdata, const uint8_t *data, size_t len);
typedef int (*nanortc_dtls_recv_fn)(void *userdata, uint8_t *buf, size_t buf_len);

/* ----------------------------------------------------------------
 * Crypto Provider
 * ---------------------------------------------------------------- */

#ifndef NANORTC_CRYPTO_PROVIDER_T_DECLARED
#define NANORTC_CRYPTO_PROVIDER_T_DECLARED
typedef struct nanortc_crypto_provider nanortc_crypto_provider_t;
#endif

struct nanortc_crypto_provider {
    const char *name; /* e.g. "mbedtls", "wolfssl", "hw-aes" */

    /* ---- DTLS (required) ---- */

    /* Allocate and initialize a DTLS context. Returns NULL on failure.
     * is_server: 1 = DTLS server (answerer), 0 = DTLS client (offerer). */
    nanortc_crypto_dtls_ctx_t *(*dtls_ctx_new)(int is_server);

    int (*dtls_set_bio)(nanortc_crypto_dtls_ctx_t *ctx, void *userdata,
                        nanortc_dtls_send_fn send_cb, nanortc_dtls_recv_fn recv_cb);
    int (*dtls_handshake)(nanortc_crypto_dtls_ctx_t *ctx);
    int (*dtls_encrypt)(nanortc_crypto_dtls_ctx_t *ctx, const uint8_t *in, size_t in_len,
                        uint8_t *out, size_t *out_len);
    int (*dtls_decrypt)(nanortc_crypto_dtls_ctx_t *ctx, const uint8_t *in, size_t in_len,
                        uint8_t *out, size_t *out_len);
    int (*dtls_export_keying_material)(nanortc_crypto_dtls_ctx_t *ctx, const char *label,
                                       size_t label_len, uint8_t *out, size_t out_len);
    int (*dtls_get_fingerprint)(nanortc_crypto_dtls_ctx_t *ctx, char *buf, size_t buf_len);
    void (*dtls_free)(nanortc_crypto_dtls_ctx_t *ctx);

    /* ---- HMAC-SHA1 (required, for STUN MESSAGE-INTEGRITY) ---- */
    void (*hmac_sha1)(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len,
                      uint8_t out[20]);

    /* ---- CSPRNG (required) ---- */
    int (*random_bytes)(uint8_t *buf, size_t len);

    /* ---- SRTP (required when media transport is enabled) ---- */
#if NANORTC_HAVE_MEDIA_TRANSPORT
    int (*aes_128_cm)(const uint8_t key[16], const uint8_t iv[16], const uint8_t *in, size_t len,
                      uint8_t *out);
    void (*hmac_sha1_80)(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len,
                         uint8_t out[10]);
#endif
};

/* Built-in crypto providers (availability depends on NANORTC_CRYPTO_* define) */
#if defined(NANORTC_CRYPTO_OPENSSL)
NANORTC_API const nanortc_crypto_provider_t *nanortc_crypto_openssl(void);
#else
NANORTC_API const nanortc_crypto_provider_t *nanortc_crypto_mbedtls(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* NANORTC_CRYPTO_H_ */
