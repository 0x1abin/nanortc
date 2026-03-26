/*
 * nanortc — Pluggable crypto provider interface
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANO_CRYPTO_H_
#define NANO_CRYPTO_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef NANO_API
#if defined(__GNUC__) || defined(__clang__)
#define NANO_API __attribute__((visibility("default")))
#else
#define NANO_API
#endif
#endif

/* Opaque DTLS context — sized by the provider implementation */
typedef struct nano_crypto_dtls_ctx nano_crypto_dtls_ctx_t;

/* BIO callbacks for Sans I/O DTLS */
typedef int (*nano_dtls_send_fn)(void *userdata, const uint8_t *data, size_t len);
typedef int (*nano_dtls_recv_fn)(void *userdata, uint8_t *buf, size_t buf_len);

/* ----------------------------------------------------------------
 * Crypto Provider
 * ---------------------------------------------------------------- */

typedef struct nano_crypto_provider {
    const char *name; /* e.g. "mbedtls", "wolfssl", "hw-aes" */

    /* ---- DTLS (required) ---- */
    int (*dtls_init)(nano_crypto_dtls_ctx_t *ctx, int is_server);
    int (*dtls_set_bio)(nano_crypto_dtls_ctx_t *ctx, void *userdata,
                        nano_dtls_send_fn send_cb, nano_dtls_recv_fn recv_cb);
    int (*dtls_handshake)(nano_crypto_dtls_ctx_t *ctx);
    int (*dtls_encrypt)(nano_crypto_dtls_ctx_t *ctx,
                        const uint8_t *in, size_t in_len,
                        uint8_t *out, size_t *out_len);
    int (*dtls_decrypt)(nano_crypto_dtls_ctx_t *ctx,
                        const uint8_t *in, size_t in_len,
                        uint8_t *out, size_t *out_len);
    int (*dtls_export_keying_material)(nano_crypto_dtls_ctx_t *ctx,
                                       const char *label,
                                       uint8_t *out, size_t out_len);
    int (*dtls_get_fingerprint)(nano_crypto_dtls_ctx_t *ctx,
                                char *buf, size_t buf_len);
    void (*dtls_free)(nano_crypto_dtls_ctx_t *ctx);

    /* ---- HMAC-SHA1 (required, for STUN MESSAGE-INTEGRITY) ---- */
    void (*hmac_sha1)(const uint8_t *key, size_t key_len,
                      const uint8_t *data, size_t data_len,
                      uint8_t out[20]);

    /* ---- CSPRNG (required) ---- */
    int (*random_bytes)(uint8_t *buf, size_t len);

    /* ---- SRTP (required for AUDIO/MEDIA profiles) ---- */
#if NANORTC_PROFILE >= NANO_PROFILE_AUDIO
    int (*aes_128_cm)(const uint8_t key[16], const uint8_t iv[16],
                      const uint8_t *in, size_t len, uint8_t *out);
    void (*hmac_sha1_80)(const uint8_t *key, size_t key_len,
                         const uint8_t *data, size_t data_len,
                         uint8_t out[10]);
#endif

} nano_crypto_provider_t;

/* Built-in crypto providers (availability depends on NANORTC_CRYPTO_* define) */
#if defined(NANORTC_CRYPTO_OPENSSL)
NANO_API const nano_crypto_provider_t *nano_crypto_openssl(void);
#else
NANO_API const nano_crypto_provider_t *nano_crypto_mbedtls(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* NANO_CRYPTO_H_ */
