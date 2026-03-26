/*
 * nanortc — DTLS tests
 *
 * Tests the DTLS state machine, BIO adapter, and crypto provider integration.
 * Core test: two-instance handshake loopback (client ↔ server in memory).
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nano_dtls.h"
#include "nano_crypto.h"
#include "nano_test.h"
#include "nano_test_config.h"
#include <string.h>

/* ----------------------------------------------------------------
 * Init tests
 * ---------------------------------------------------------------- */

TEST(test_dtls_init_server)
{
    nano_dtls_t dtls;
    int rc = dtls_init(&dtls, nano_test_crypto(), 1);
    ASSERT_OK(rc);
    ASSERT_EQ(dtls.state, NANO_DTLS_STATE_INIT);
    ASSERT_TRUE(dtls.crypto_ctx != NULL);
    ASSERT_TRUE(dtls.is_server == 1);
    dtls_destroy(&dtls);
    ASSERT_EQ(dtls.state, NANO_DTLS_STATE_CLOSED);
}

TEST(test_dtls_init_client)
{
    nano_dtls_t dtls;
    int rc = dtls_init(&dtls, nano_test_crypto(), 0);
    ASSERT_OK(rc);
    ASSERT_EQ(dtls.state, NANO_DTLS_STATE_INIT);
    ASSERT_TRUE(dtls.is_server == 0);
    dtls_destroy(&dtls);
}

TEST(test_dtls_init_null_params)
{
    nano_dtls_t dtls;
    ASSERT_FAIL(dtls_init(NULL, nano_test_crypto(), 0));
    ASSERT_FAIL(dtls_init(&dtls, NULL, 0));
}

/* ----------------------------------------------------------------
 * Fingerprint tests
 * ---------------------------------------------------------------- */

TEST(test_dtls_fingerprint_format)
{
    nano_dtls_t dtls;
    dtls_init(&dtls, nano_test_crypto(), 0);

    const char *fp = dtls_get_fingerprint(&dtls);
    ASSERT_TRUE(fp != NULL);

    /* SHA-256 fingerprint: 32 hex pairs separated by colons = 95 chars */
    size_t len = strlen(fp);
    ASSERT_EQ(len, (size_t)95);

    /* Verify format: XX:XX:XX:... */
    for (size_t i = 0; i < len; i++) {
        if ((i + 1) % 3 == 0) {
            ASSERT_EQ(fp[i], ':');
        } else {
            ASSERT_TRUE((fp[i] >= '0' && fp[i] <= '9') || (fp[i] >= 'A' && fp[i] <= 'F'));
        }
    }

    dtls_destroy(&dtls);
}

TEST(test_dtls_fingerprint_unique)
{
    /* Two instances should generate different certificates → different fingerprints */
    nano_dtls_t dtls1, dtls2;
    dtls_init(&dtls1, nano_test_crypto(), 0);
    dtls_init(&dtls2, nano_test_crypto(), 0);

    const char *fp1 = dtls_get_fingerprint(&dtls1);
    const char *fp2 = dtls_get_fingerprint(&dtls2);
    ASSERT_TRUE(fp1 != NULL);
    ASSERT_TRUE(fp2 != NULL);
    ASSERT_TRUE(strcmp(fp1, fp2) != 0);

    dtls_destroy(&dtls1);
    dtls_destroy(&dtls2);
}

/* ----------------------------------------------------------------
 * Handshake loopback test
 * ---------------------------------------------------------------- */

/*
 * Relay DTLS records between client and server via in-memory buffers.
 * This is the Sans I/O equivalent of a UDP socket pair.
 */
static int dtls_relay(nano_dtls_t *from, nano_dtls_t *to)
{
    uint8_t buf[4096];
    size_t len = 0;
    int relayed = 0;

    while (dtls_poll_output(from, buf, sizeof(buf), &len) == NANO_OK && len > 0) {
        int rc = dtls_handle_data(to, buf, len);
        if (rc < 0) {
            return rc;
        }
        relayed++;
        len = 0;
    }
    return relayed;
}

TEST(test_dtls_handshake_loopback)
{
    nano_dtls_t server, client;
    const nano_crypto_provider_t *crypto = nano_test_crypto();

    /* Initialize both sides */
    ASSERT_OK(dtls_init(&server, crypto, 1));
    ASSERT_OK(dtls_init(&client, crypto, 0));

    /* Client initiates handshake (sends ClientHello) */
    ASSERT_OK(dtls_start(&client));
    ASSERT_EQ(client.state, NANO_DTLS_STATE_HANDSHAKING);

    /* Pump: relay DTLS records back and forth until both are established
     * DTLS 1.2 handshake typically takes 4-6 round trips */
    int established = 0;
    for (int round = 0; round < 30; round++) {
        /* client → server */
        dtls_relay(&client, &server);

        /* server → client */
        dtls_relay(&server, &client);

        if (server.state == NANO_DTLS_STATE_ESTABLISHED &&
            client.state == NANO_DTLS_STATE_ESTABLISHED) {
            established = 1;
            break;
        }
    }

    ASSERT_TRUE(established);
    ASSERT_EQ(server.state, NANO_DTLS_STATE_ESTABLISHED);
    ASSERT_EQ(client.state, NANO_DTLS_STATE_ESTABLISHED);

    dtls_destroy(&server);
    dtls_destroy(&client);
}

/* ----------------------------------------------------------------
 * Encrypt / decrypt test
 * ---------------------------------------------------------------- */

/* Helper: complete a handshake between client and server */
static int dtls_do_handshake(nano_dtls_t *client, nano_dtls_t *server)
{
    dtls_start(client);
    for (int round = 0; round < 30; round++) {
        dtls_relay(client, server);
        dtls_relay(server, client);
        if (server->state == NANO_DTLS_STATE_ESTABLISHED &&
            client->state == NANO_DTLS_STATE_ESTABLISHED) {
            return 0;
        }
    }
    return -1;
}

TEST(test_dtls_encrypt_decrypt)
{
    nano_dtls_t server, client;
    const nano_crypto_provider_t *crypto = nano_test_crypto();
    ASSERT_OK(dtls_init(&server, crypto, 1));
    ASSERT_OK(dtls_init(&client, crypto, 0));
    ASSERT_OK(dtls_do_handshake(&client, &server));

    /* Client encrypts a message */
    const uint8_t plaintext[] = "Hello, DTLS!";
    ASSERT_OK(dtls_encrypt(&client, plaintext, sizeof(plaintext)));

    /* Relay encrypted record to server */
    dtls_relay(&client, &server);

    /* Server should have decrypted data in app_buf */
    const uint8_t *app_data = NULL;
    size_t app_len = 0;
    int rc = dtls_poll_app_data(&server, &app_data, &app_len);
    ASSERT_OK(rc);
    ASSERT_EQ(app_len, sizeof(plaintext));
    ASSERT_MEM_EQ(app_data, plaintext, sizeof(plaintext));

    /* Server encrypts a response */
    const uint8_t response[] = "Hello back!";
    ASSERT_OK(dtls_encrypt(&server, response, sizeof(response)));

    /* Relay to client */
    dtls_relay(&server, &client);

    rc = dtls_poll_app_data(&client, &app_data, &app_len);
    ASSERT_OK(rc);
    ASSERT_EQ(app_len, sizeof(response));
    ASSERT_MEM_EQ(app_data, response, sizeof(response));

    dtls_destroy(&server);
    dtls_destroy(&client);
}

/* ----------------------------------------------------------------
 * Keying material test
 * ---------------------------------------------------------------- */

TEST(test_dtls_keying_material)
{
    nano_dtls_t server, client;
    const nano_crypto_provider_t *crypto = nano_test_crypto();
    ASSERT_OK(dtls_init(&server, crypto, 1));
    ASSERT_OK(dtls_init(&client, crypto, 0));
    ASSERT_OK(dtls_do_handshake(&client, &server));

    /* Both sides should have exported keying material */
    ASSERT_TRUE(server.keying_material_ready);
    ASSERT_TRUE(client.keying_material_ready);

    /* Both sides should derive the same keying material */
    ASSERT_MEM_EQ(server.keying_material, client.keying_material, 60);

    /* Keying material should not be all zeros */
    uint8_t zeros[60];
    memset(zeros, 0, sizeof(zeros));
    ASSERT_TRUE(memcmp(server.keying_material, zeros, 60) != 0);

    dtls_destroy(&server);
    dtls_destroy(&client);
}

/* ----------------------------------------------------------------
 * State validation tests
 * ---------------------------------------------------------------- */

TEST(test_dtls_wrong_state)
{
    nano_dtls_t dtls;
    ASSERT_OK(dtls_init(&dtls, nano_test_crypto(), 0));

    /* Cannot encrypt before handshake */
    const uint8_t data[] = "test";
    ASSERT_FAIL(dtls_encrypt(&dtls, data, sizeof(data)));

    /* Cannot start twice */
    ASSERT_OK(dtls_start(&dtls));
    ASSERT_FAIL(dtls_start(&dtls));

    dtls_destroy(&dtls);
}

/* ----------------------------------------------------------------
 * Test main
 * ---------------------------------------------------------------- */

TEST_MAIN_BEGIN("test_dtls")
RUN(test_dtls_init_server);
RUN(test_dtls_init_client);
RUN(test_dtls_init_null_params);
RUN(test_dtls_fingerprint_format);
RUN(test_dtls_fingerprint_unique);
RUN(test_dtls_handshake_loopback);
RUN(test_dtls_encrypt_decrypt);
RUN(test_dtls_keying_material);
RUN(test_dtls_wrong_state);
TEST_MAIN_END
