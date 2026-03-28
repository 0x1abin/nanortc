/*
 * nanortc — SRTP tests (RFC 3711)
 *
 * Test vectors from RFC 3711 Appendix B.3 for key derivation.
 * Roundtrip protect/unprotect verification.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nano_srtp.h"
#include "nano_rtp.h"
#include "nanortc_crypto.h"
#include "nano_test.h"
#include "nano_test_config.h"
#include <string.h>

#if NANORTC_HAVE_MEDIA_TRANSPORT

/* ---- RFC 3711 Appendix B.3 key derivation test vectors ---- */

/* Master key: E1F97A0D3E018BE0D64FA32C06DE4139 */
static const uint8_t rfc3711_master_key[16] = {
    0xE1, 0xF9, 0x7A, 0x0D, 0x3E, 0x01, 0x8B, 0xE0,
    0xD6, 0x4F, 0xA3, 0x2C, 0x06, 0xDE, 0x41, 0x39
};

/* Master salt: 0EC675AD498AFEEBB6960B3AABE6 */
static const uint8_t rfc3711_master_salt[14] = {
    0x0E, 0xC6, 0x75, 0xAD, 0x49, 0x8A, 0xFE, 0xEB,
    0xB6, 0x96, 0x0B, 0x3A, 0xAB, 0xE6
};

/* Expected session encryption key (label=0): C61E7A93744F39EE10734AFE3FF7A087 */
static const uint8_t rfc3711_session_key[16] = {
    0xC6, 0x1E, 0x7A, 0x93, 0x74, 0x4F, 0x39, 0xEE,
    0x10, 0x73, 0x4A, 0xFE, 0x3F, 0xF7, 0xA0, 0x87
};

/* Expected session auth key (label=1): CEBE321F6FF7716B6FD4AB49AF256A156D38BAA4 */
static const uint8_t rfc3711_session_auth_key[20] = {
    0xCE, 0xBE, 0x32, 0x1F, 0x6F, 0xF7, 0x71, 0x6B,
    0x6F, 0xD4, 0xAB, 0x49, 0xAF, 0x25, 0x6A, 0x15,
    0x6D, 0x38, 0xBA, 0xA4
};

/* Expected session salt (label=2): 30CBBC08863D8C85D49DB34A9AE1 */
static const uint8_t rfc3711_session_salt[14] = {
    0x30, 0xCB, 0xBC, 0x08, 0x86, 0x3D, 0x8C, 0x85,
    0xD4, 0x9D, 0xB3, 0x4A, 0x9A, 0xE1
};

/*
 * Test key derivation against RFC 3711 B.3 vectors.
 *
 * We construct synthetic keying_material where the client side holds
 * the RFC test master key and salt, then verify the derived session
 * keys match the expected values.
 */
TEST(test_srtp_key_derivation_rfc3711_b3)
{
    const nanortc_crypto_provider_t *crypto = nano_test_crypto();

    /* Build 60-byte keying material:
     * client_key(16) | server_key(16) | client_salt(14) | server_salt(14) */
    uint8_t km[60];
    memcpy(km, rfc3711_master_key, 16);       /* client_key */
    memset(km + 16, 0xAA, 16);                /* server_key (don't care) */
    memcpy(km + 32, rfc3711_master_salt, 14); /* client_salt */
    memset(km + 46, 0xBB, 14);                /* server_salt (don't care) */

    nano_srtp_t srtp;
    srtp_init(&srtp, crypto, 1); /* is_client=1 → send uses client keys */
    ASSERT_OK(srtp_derive_keys(&srtp, km, sizeof(km)));

    /* Verify send (client) session keys match RFC test vectors */
    ASSERT_MEM_EQ(srtp.send_key, rfc3711_session_key, 16);
    ASSERT_MEM_EQ(srtp.send_auth_key, rfc3711_session_auth_key, 20);
    ASSERT_MEM_EQ(srtp.send_salt, rfc3711_session_salt, 14);

    ASSERT_TRUE(srtp.ready);
}

/*
 * Verify client/server key direction: DTLS server should use
 * server keys for sending, client keys for receiving.
 */
TEST(test_srtp_key_direction)
{
    const nanortc_crypto_provider_t *crypto = nano_test_crypto();

    uint8_t km[60];
    memcpy(km, rfc3711_master_key, 16);       /* client_key */
    memset(km + 16, 0x42, 16);                /* server_key */
    memcpy(km + 32, rfc3711_master_salt, 14); /* client_salt */
    memset(km + 46, 0x55, 14);                /* server_salt */

    nano_srtp_t client, server;
    srtp_init(&client, crypto, 1); /* DTLS client */
    srtp_init(&server, crypto, 0); /* DTLS server */

    ASSERT_OK(srtp_derive_keys(&client, km, sizeof(km)));
    ASSERT_OK(srtp_derive_keys(&server, km, sizeof(km)));

    /* Client send keys = server recv keys */
    ASSERT_MEM_EQ(client.send_key, server.recv_key, 16);
    ASSERT_MEM_EQ(client.send_auth_key, server.recv_auth_key, 20);
    ASSERT_MEM_EQ(client.send_salt, server.recv_salt, 14);

    /* Server send keys = client recv keys */
    ASSERT_MEM_EQ(server.send_key, client.recv_key, 16);
    ASSERT_MEM_EQ(server.send_auth_key, client.recv_auth_key, 20);
    ASSERT_MEM_EQ(server.send_salt, client.recv_salt, 14);
}

/*
 * Roundtrip: protect → unprotect with paired client/server contexts.
 */
TEST(test_srtp_protect_unprotect_roundtrip)
{
    const nanortc_crypto_provider_t *crypto = nano_test_crypto();

    /* Generate random keying material */
    uint8_t km[60];
    crypto->random_bytes(km, sizeof(km));

    nano_srtp_t sender, receiver;
    srtp_init(&sender, crypto, 1);   /* DTLS client sends */
    srtp_init(&receiver, crypto, 0); /* DTLS server receives */

    ASSERT_OK(srtp_derive_keys(&sender, km, sizeof(km)));
    ASSERT_OK(srtp_derive_keys(&receiver, km, sizeof(km)));

    /* Build an RTP packet */
    nano_rtp_t rtp;
    rtp_init(&rtp, 0xDEADBEEF, 111);

    const uint8_t payload[] = "hello srtp world";
    /* Buffer needs room for RTP header + payload + SRTP auth tag + 4 bytes ROC scratch */
    uint8_t buf[256];
    size_t pkt_len;
    ASSERT_OK(rtp_pack(&rtp, 48000, payload, sizeof(payload) - 1,
                       buf, sizeof(buf), &pkt_len));

    /* Save original packet for comparison */
    uint8_t original[256];
    memcpy(original, buf, pkt_len);
    size_t original_len = pkt_len;

    /* Protect (encrypt + authenticate) */
    size_t srtp_len;
    ASSERT_OK(srtp_protect(&sender, buf, pkt_len, &srtp_len));
    ASSERT_EQ(srtp_len, pkt_len + 10); /* +10 auth tag */

    /* Encrypted payload should differ from original */
    ASSERT_TRUE(memcmp(buf + 12, original + 12, original_len - 12) != 0);

    /* Unprotect (verify + decrypt) */
    size_t rtp_len;
    ASSERT_OK(srtp_unprotect(&receiver, buf, srtp_len, &rtp_len));
    ASSERT_EQ(rtp_len, original_len);

    /* Decrypted packet should match original */
    ASSERT_MEM_EQ(buf, original, original_len);
}

/*
 * Verify authentication: tampered packet should fail unprotect.
 */
TEST(test_srtp_tamper_detection)
{
    const nanortc_crypto_provider_t *crypto = nano_test_crypto();

    uint8_t km[60];
    crypto->random_bytes(km, sizeof(km));

    nano_srtp_t sender, receiver;
    srtp_init(&sender, crypto, 1);
    srtp_init(&receiver, crypto, 0);
    ASSERT_OK(srtp_derive_keys(&sender, km, sizeof(km)));
    ASSERT_OK(srtp_derive_keys(&receiver, km, sizeof(km)));

    nano_rtp_t rtp;
    rtp_init(&rtp, 1, 96);

    uint8_t buf[256];
    size_t pkt_len, srtp_len, out_len;
    ASSERT_OK(rtp_pack(&rtp, 0, (const uint8_t *)"test", 4, buf, sizeof(buf), &pkt_len));
    ASSERT_OK(srtp_protect(&sender, buf, pkt_len, &srtp_len));

    /* Tamper with encrypted payload */
    buf[12] ^= 0xFF;

    /* Unprotect should fail due to auth mismatch */
    ASSERT_EQ(srtp_unprotect(&receiver, buf, srtp_len, &out_len), NANORTC_ERR_CRYPTO);
}

/*
 * Multiple packets: verify sequence number tracking works.
 */
TEST(test_srtp_multiple_packets)
{
    const nanortc_crypto_provider_t *crypto = nano_test_crypto();

    uint8_t km[60];
    crypto->random_bytes(km, sizeof(km));

    nano_srtp_t sender, receiver;
    srtp_init(&sender, crypto, 1);
    srtp_init(&receiver, crypto, 0);
    ASSERT_OK(srtp_derive_keys(&sender, km, sizeof(km)));
    ASSERT_OK(srtp_derive_keys(&receiver, km, sizeof(km)));

    nano_rtp_t rtp;
    rtp_init(&rtp, 0x42, 111);

    int i;
    for (i = 0; i < 10; i++) {
        uint8_t payload[4] = {(uint8_t)i, (uint8_t)(i + 1), (uint8_t)(i + 2), (uint8_t)(i + 3)};
        uint8_t buf[256];
        size_t pkt_len, srtp_len, rtp_len;

        ASSERT_OK(rtp_pack(&rtp, (uint32_t)(i * 960), payload, 4,
                           buf, sizeof(buf), &pkt_len));
        ASSERT_OK(srtp_protect(&sender, buf, pkt_len, &srtp_len));
        ASSERT_OK(srtp_unprotect(&receiver, buf, srtp_len, &rtp_len));

        /* Verify payload recovered */
        ASSERT_MEM_EQ(buf + 12, payload, 4);
    }
}

/*
 * Test AES-128-CM crypto primitive directly with a known vector.
 */
TEST(test_aes_128_cm_basic)
{
    const nanortc_crypto_provider_t *crypto = nano_test_crypto();

    /* Encrypt zeros with known key/IV, decrypt result, should get zeros back */
    uint8_t key[16] = {0x2B, 0x7E, 0x15, 0x16, 0x28, 0xAE, 0xD2, 0xA6,
                       0xAB, 0xF7, 0x15, 0x88, 0x09, 0xCF, 0x4F, 0x3C};
    uint8_t iv[16];
    memset(iv, 0, 16);

    uint8_t zeros[16];
    memset(zeros, 0, 16);

    uint8_t encrypted[16];
    ASSERT_OK(crypto->aes_128_cm(key, iv, zeros, 16, encrypted));

    /* Encrypt again (CTR XOR is self-inverse) */
    uint8_t decrypted[16];
    ASSERT_OK(crypto->aes_128_cm(key, iv, encrypted, 16, decrypted));
    ASSERT_MEM_EQ(decrypted, zeros, 16);
}

#endif /* NANORTC_HAVE_MEDIA_TRANSPORT */

/* ---- Runner ---- */

TEST_MAIN_BEGIN("SRTP tests")
#if NANORTC_HAVE_MEDIA_TRANSPORT
    RUN(test_srtp_key_derivation_rfc3711_b3);
    RUN(test_srtp_key_direction);
    RUN(test_srtp_protect_unprotect_roundtrip);
    RUN(test_srtp_tamper_detection);
    RUN(test_srtp_multiple_packets);
    RUN(test_aes_128_cm_basic);
#endif
TEST_MAIN_END
