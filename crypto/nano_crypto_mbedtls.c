/*
 * nanortc — mbedtls crypto provider (stub)
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_crypto.h"

/* ---- Stub implementations ---- */

static int stub_dtls_init(nano_crypto_dtls_ctx_t *ctx, int is_server)
{
    (void)ctx;
    (void)is_server;
    return -1; /* NANO_ERR_NOT_IMPLEMENTED */
}

static int stub_dtls_set_bio(nano_crypto_dtls_ctx_t *ctx, void *userdata,
                             nano_dtls_send_fn send_cb, nano_dtls_recv_fn recv_cb)
{
    (void)ctx;
    (void)userdata;
    (void)send_cb;
    (void)recv_cb;
    return -1;
}

static int stub_dtls_handshake(nano_crypto_dtls_ctx_t *ctx)
{
    (void)ctx;
    return -1;
}

static int stub_dtls_encrypt(nano_crypto_dtls_ctx_t *ctx,
                             const uint8_t *in, size_t in_len,
                             uint8_t *out, size_t *out_len)
{
    (void)ctx;
    (void)in;
    (void)in_len;
    (void)out;
    (void)out_len;
    return -1;
}

static int stub_dtls_decrypt(nano_crypto_dtls_ctx_t *ctx,
                             const uint8_t *in, size_t in_len,
                             uint8_t *out, size_t *out_len)
{
    (void)ctx;
    (void)in;
    (void)in_len;
    (void)out;
    (void)out_len;
    return -1;
}

static int stub_dtls_export_keying_material(nano_crypto_dtls_ctx_t *ctx,
                                            const char *label,
                                            uint8_t *out, size_t out_len)
{
    (void)ctx;
    (void)label;
    (void)out;
    (void)out_len;
    return -1;
}

static int stub_dtls_get_fingerprint(nano_crypto_dtls_ctx_t *ctx,
                                     char *buf, size_t buf_len)
{
    (void)ctx;
    (void)buf;
    (void)buf_len;
    return -1;
}

static void stub_dtls_free(nano_crypto_dtls_ctx_t *ctx)
{
    (void)ctx;
}

static void stub_hmac_sha1(const uint8_t *key, size_t key_len,
                           const uint8_t *data, size_t data_len,
                           uint8_t out[20])
{
    (void)key;
    (void)key_len;
    (void)data;
    (void)data_len;
    (void)out;
}

static int stub_random_bytes(uint8_t *buf, size_t len)
{
    (void)buf;
    (void)len;
    return -1;
}

#if NANORTC_PROFILE >= NANO_PROFILE_AUDIO
static int stub_aes_128_cm(const uint8_t key[16], const uint8_t iv[16],
                           const uint8_t *in, size_t len, uint8_t *out)
{
    (void)key;
    (void)iv;
    (void)in;
    (void)len;
    (void)out;
    return -1;
}

static void stub_hmac_sha1_80(const uint8_t *key, size_t key_len,
                              const uint8_t *data, size_t data_len,
                              uint8_t out[10])
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
    .name = "mbedtls-stub",
    .dtls_init = stub_dtls_init,
    .dtls_set_bio = stub_dtls_set_bio,
    .dtls_handshake = stub_dtls_handshake,
    .dtls_encrypt = stub_dtls_encrypt,
    .dtls_decrypt = stub_dtls_decrypt,
    .dtls_export_keying_material = stub_dtls_export_keying_material,
    .dtls_get_fingerprint = stub_dtls_get_fingerprint,
    .dtls_free = stub_dtls_free,
    .hmac_sha1 = stub_hmac_sha1,
    .random_bytes = stub_random_bytes,
#if NANORTC_PROFILE >= NANO_PROFILE_AUDIO
    .aes_128_cm = stub_aes_128_cm,
    .hmac_sha1_80 = stub_hmac_sha1_80,
#endif
};

const nano_crypto_provider_t *nano_crypto_mbedtls(void)
{
    return &mbedtls_provider;
}
