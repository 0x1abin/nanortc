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

/* ---- DTLS stubs (Phase 1 Step 2) ---- */

static int stub_dtls_init(nano_crypto_dtls_ctx_t *ctx, int is_server)
{
    (void)ctx;
    (void)is_server;
    return -1;
}

static int stub_dtls_set_bio(nano_crypto_dtls_ctx_t *ctx, void *userdata, nano_dtls_send_fn send_cb,
                             nano_dtls_recv_fn recv_cb)
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

static int stub_dtls_encrypt(nano_crypto_dtls_ctx_t *ctx, const uint8_t *in, size_t in_len,
                             uint8_t *out, size_t *out_len)
{
    (void)ctx;
    (void)in;
    (void)in_len;
    (void)out;
    (void)out_len;
    return -1;
}

static int stub_dtls_decrypt(nano_crypto_dtls_ctx_t *ctx, const uint8_t *in, size_t in_len,
                             uint8_t *out, size_t *out_len)
{
    (void)ctx;
    (void)in;
    (void)in_len;
    (void)out;
    (void)out_len;
    return -1;
}

static int stub_dtls_export_keying_material(nano_crypto_dtls_ctx_t *ctx, const char *label,
                                            uint8_t *out, size_t out_len)
{
    (void)ctx;
    (void)label;
    (void)out;
    (void)out_len;
    return -1;
}

static int stub_dtls_get_fingerprint(nano_crypto_dtls_ctx_t *ctx, char *buf, size_t buf_len)
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

static const nano_crypto_provider_t openssl_provider = {
    .name = "openssl",
    .dtls_init = stub_dtls_init,
    .dtls_set_bio = stub_dtls_set_bio,
    .dtls_handshake = stub_dtls_handshake,
    .dtls_encrypt = stub_dtls_encrypt,
    .dtls_decrypt = stub_dtls_decrypt,
    .dtls_export_keying_material = stub_dtls_export_keying_material,
    .dtls_get_fingerprint = stub_dtls_get_fingerprint,
    .dtls_free = stub_dtls_free,
    .hmac_sha1 = ossl_hmac_sha1,
    .random_bytes = ossl_random_bytes,
#if NANORTC_PROFILE >= NANO_PROFILE_AUDIO
    .aes_128_cm = stub_aes_128_cm,
    .hmac_sha1_80 = stub_hmac_sha1_80,
#endif
};

const nano_crypto_provider_t *nano_crypto_openssl(void)
{
    return &openssl_provider;
}
