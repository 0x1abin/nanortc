/*
 * nanortc interop tests — DataChannel interoperability with libdatachannel
 *
 * Test topology:
 *   libdatachannel (offerer/controlling) <--localhost UDP--> nanortc (answerer/controlled)
 *
 * Signaling is exchanged via a socketpair within the same process.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_test.h"
#include "interop_common.h"
#include "interop_nanortc_peer.h"
#include "interop_libdc_peer.h"

#include <stdio.h>
#include <string.h>

/* ----------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------- */

/* Port counter to avoid bind conflicts between tests */
static uint16_t next_port = INTEROP_PORT_BASE;

static uint16_t alloc_port(void)
{
    return next_port++;
}

/*
 * Full setup: create signaling pipe, start both peers, wait for
 * DataChannel to open on both sides.
 *
 * On success, caller must call teardown() to clean up.
 */
static int setup_connected_pair(interop_sig_pipe_t *pipe,
                                interop_nanortc_peer_t *nano,
                                interop_libdc_peer_t *libdc,
                                const char *dc_label)
{
    uint16_t port = alloc_port();

    if (interop_sig_create(pipe) != 0) {
        fprintf(stderr, "[test] Failed to create signaling pipe\n");
        return -1;
    }

    /* Start nanortc peer first (it waits for offer) */
    if (interop_nanortc_start(nano, pipe->fd[0], port) != 0) {
        fprintf(stderr, "[test] Failed to start nanortc peer\n");
        interop_sig_destroy(pipe);
        return -1;
    }

    /* Start libdatachannel peer (generates offer, exchanges signaling) */
    if (interop_libdc_start(libdc, pipe->fd[1], dc_label, port) != 0) {
        fprintf(stderr, "[test] Failed to start libdc peer\n");
        interop_nanortc_stop(nano);
        interop_sig_destroy(pipe);
        return -1;
    }

    return 0;
}

static void teardown_pair(interop_sig_pipe_t *pipe,
                          interop_nanortc_peer_t *nano,
                          interop_libdc_peer_t *libdc)
{
    interop_libdc_stop(libdc);
    interop_nanortc_stop(nano);
    interop_sig_destroy(pipe);
}

/* ----------------------------------------------------------------
 * Test: Full handshake (ICE + DTLS + SCTP)
 * ---------------------------------------------------------------- */

TEST(test_interop_handshake)
{
    interop_sig_pipe_t pipe;
    interop_nanortc_peer_t nano;
    interop_libdc_peer_t libdc;

    int rc = setup_connected_pair(&pipe, &nano, &libdc, "test");
    ASSERT_OK(rc);

    /* Wait for connection on both sides */
    rc = interop_libdc_wait_flag(&libdc.connected, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    rc = interop_nanortc_wait_flag(&nano.sctp_connected, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    /* Verify nanortc passed through all connection stages */
    ASSERT_TRUE(atomic_load(&nano.ice_connected));
    ASSERT_TRUE(atomic_load(&nano.dtls_connected));
    ASSERT_TRUE(atomic_load(&nano.sctp_connected));

    teardown_pair(&pipe, &nano, &libdc);
}

/* ----------------------------------------------------------------
 * Test: DataChannel opens on both sides
 * ---------------------------------------------------------------- */

TEST(test_interop_dc_open)
{
    interop_sig_pipe_t pipe;
    interop_nanortc_peer_t nano;
    interop_libdc_peer_t libdc;

    int rc = setup_connected_pair(&pipe, &nano, &libdc, "interop-dc");
    ASSERT_OK(rc);

    /* Wait for DC open on both sides */
    rc = interop_libdc_wait_flag(&libdc.dc_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    rc = interop_nanortc_wait_flag(&nano.dc_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    teardown_pair(&pipe, &nano, &libdc);
}

/* ----------------------------------------------------------------
 * Test: String message from libdatachannel to nanortc
 * ---------------------------------------------------------------- */

TEST(test_interop_dc_string_libdc_to_nano)
{
    interop_sig_pipe_t pipe;
    interop_nanortc_peer_t nano;
    interop_libdc_peer_t libdc;

    int rc = setup_connected_pair(&pipe, &nano, &libdc, "echo");
    ASSERT_OK(rc);

    /* Wait for DC open */
    rc = interop_libdc_wait_flag(&libdc.dc_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_nanortc_wait_flag(&nano.dc_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    /* Send string from libdc */
    const char *msg = "hello nanortc";
    int initial_count = atomic_load(&nano.msg_count);
    rc = interop_libdc_send_string(&libdc, msg);
    ASSERT_TRUE(rc >= 0);

    /* Wait for nanortc to receive it */
    uint32_t start = interop_get_millis();
    while (atomic_load(&nano.msg_count) <= initial_count) {
        ASSERT_TRUE(interop_get_millis() - start < INTEROP_TIMEOUT_MS);
        interop_sleep_ms(10);
    }

    /* Verify */
    pthread_mutex_lock(&nano.msg_mutex);
    ASSERT_TRUE(nano.last_msg_is_string);
    ASSERT_EQ(nano.last_msg_len, strlen(msg));
    ASSERT_TRUE(memcmp(nano.last_msg, msg, strlen(msg)) == 0);
    pthread_mutex_unlock(&nano.msg_mutex);

    teardown_pair(&pipe, &nano, &libdc);
}

/* ----------------------------------------------------------------
 * Test: String message from nanortc to libdatachannel
 * ---------------------------------------------------------------- */

TEST(test_interop_dc_string_nano_to_libdc)
{
    interop_sig_pipe_t pipe;
    interop_nanortc_peer_t nano;
    interop_libdc_peer_t libdc;

    int rc = setup_connected_pair(&pipe, &nano, &libdc, "echo");
    ASSERT_OK(rc);

    /* Wait for DC open */
    rc = interop_libdc_wait_flag(&libdc.dc_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_nanortc_wait_flag(&nano.dc_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    /* Send string from nanortc (need to call from nanortc context) */
    const char *msg = "hello libdc";
    int initial_count = atomic_load(&libdc.msg_count);

    /* nano_send_datachannel_string is thread-safe for our purposes here
     * because the run_loop is the only other accessor of rtc */
    rc = nano_send_datachannel_string(&nano.rtc, 0, msg);
    ASSERT_OK(rc);

    /* Wait for libdc to receive it */
    uint32_t start = interop_get_millis();
    while (atomic_load(&libdc.msg_count) <= initial_count) {
        ASSERT_TRUE(interop_get_millis() - start < INTEROP_TIMEOUT_MS);
        interop_sleep_ms(10);
    }

    /* Verify */
    pthread_mutex_lock(&libdc.msg_mutex);
    ASSERT_TRUE(libdc.last_msg_is_string);
    ASSERT_EQ(libdc.last_msg_len, strlen(msg));
    ASSERT_TRUE(memcmp(libdc.last_msg, msg, strlen(msg)) == 0);
    pthread_mutex_unlock(&libdc.msg_mutex);

    teardown_pair(&pipe, &nano, &libdc);
}

/* ----------------------------------------------------------------
 * Test: Binary message round-trip
 * ---------------------------------------------------------------- */

TEST(test_interop_dc_binary)
{
    interop_sig_pipe_t pipe;
    interop_nanortc_peer_t nano;
    interop_libdc_peer_t libdc;

    int rc = setup_connected_pair(&pipe, &nano, &libdc, "binary");
    ASSERT_OK(rc);

    rc = interop_libdc_wait_flag(&libdc.dc_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_nanortc_wait_flag(&nano.dc_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    /* Prepare binary payload (256 bytes, pattern fill) */
    uint8_t payload[256];
    for (int i = 0; i < 256; i++) {
        payload[i] = (uint8_t)i;
    }

    int initial_count = atomic_load(&nano.msg_count);
    rc = interop_libdc_send_binary(&libdc, payload, sizeof(payload));
    ASSERT_TRUE(rc >= 0);

    /* Wait for nanortc to receive */
    uint32_t start = interop_get_millis();
    while (atomic_load(&nano.msg_count) <= initial_count) {
        ASSERT_TRUE(interop_get_millis() - start < INTEROP_TIMEOUT_MS);
        interop_sleep_ms(10);
    }

    /* Verify */
    pthread_mutex_lock(&nano.msg_mutex);
    ASSERT_FALSE(nano.last_msg_is_string);
    ASSERT_EQ(nano.last_msg_len, sizeof(payload));
    ASSERT_TRUE(memcmp(nano.last_msg, payload, sizeof(payload)) == 0);
    pthread_mutex_unlock(&nano.msg_mutex);

    teardown_pair(&pipe, &nano, &libdc);
}

/* ----------------------------------------------------------------
 * Test runner
 * ---------------------------------------------------------------- */

TEST_MAIN_BEGIN("interop-datachannel")
    RUN(test_interop_handshake);
    RUN(test_interop_dc_open);
    RUN(test_interop_dc_string_libdc_to_nano);
    RUN(test_interop_dc_string_nano_to_libdc);
    RUN(test_interop_dc_binary);
TEST_MAIN_END
