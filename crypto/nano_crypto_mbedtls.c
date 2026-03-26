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

/* ---- HMAC-SHA1 (for STUN MESSAGE-INTEGRITY, RFC 8489 §14.5) ---- */

static void mbed_hmac_sha1(const uint8_t *key, size_t key_len,
                            const uint8_t *data, size_t data_len,
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
    int ret = mbedtls_ctr_drbg_seed(&mbed_ctr_drbg, mbedtls_entropy_func,
                                     &mbed_entropy,
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

/* ---- DTLS stubs (Phase 1 Step 2) ---- */

static int stub_dtls_init(nano_crypto_dtls_ctx_t *ctx, int is_server)
{
    (void)ctx;
    (void)is_server;
    return -1;
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
    .name = "mbedtls",
    .dtls_init = stub_dtls_init,
    .dtls_set_bio = stub_dtls_set_bio,
    .dtls_handshake = stub_dtls_handshake,
    .dtls_encrypt = stub_dtls_encrypt,
    .dtls_decrypt = stub_dtls_decrypt,
    .dtls_export_keying_material = stub_dtls_export_keying_material,
    .dtls_get_fingerprint = stub_dtls_get_fingerprint,
    .dtls_free = stub_dtls_free,
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
