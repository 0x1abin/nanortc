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
#include "interop_libdatachannel_peer.h"

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
static int setup_connected_pair(interop_sig_pipe_t *pipe, interop_nanortc_peer_t *nano,
                                interop_libdatachannel_peer_t *libdatachannel, const char *dc_label)
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
    if (interop_libdatachannel_start(libdatachannel, pipe->fd[1], dc_label, port) != 0) {
        fprintf(stderr, "[test] Failed to start libdatachannel peer\n");
        interop_nanortc_stop(nano);
        interop_sig_destroy(pipe);
        return -1;
    }

    return 0;
}

static void teardown_pair(interop_sig_pipe_t *pipe, interop_nanortc_peer_t *nano,
                          interop_libdatachannel_peer_t *libdatachannel)
{
    interop_libdatachannel_stop(libdatachannel);
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
    interop_libdatachannel_peer_t libdatachannel;

    int rc = setup_connected_pair(&pipe, &nano, &libdatachannel, "test");
    ASSERT_OK(rc);

    /* Wait for connection on both sides */
    rc = interop_libdatachannel_wait_flag(&libdatachannel.connected, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    rc = interop_nanortc_wait_flag(&nano.sctp_connected, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    /* Verify nanortc passed through all connection stages */
    ASSERT_TRUE(atomic_load(&nano.ice_connected));
    ASSERT_TRUE(atomic_load(&nano.dtls_connected));
    ASSERT_TRUE(atomic_load(&nano.sctp_connected));

    teardown_pair(&pipe, &nano, &libdatachannel);
}

/* ----------------------------------------------------------------
 * Test: DataChannel opens on both sides
 * ---------------------------------------------------------------- */

TEST(test_interop_dc_open)
{
    interop_sig_pipe_t pipe;
    interop_nanortc_peer_t nano;
    interop_libdatachannel_peer_t libdatachannel;

    int rc = setup_connected_pair(&pipe, &nano, &libdatachannel, "interop-dc");
    ASSERT_OK(rc);

    /* Wait for DC open on both sides */
    rc = interop_libdatachannel_wait_flag(&libdatachannel.dc_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    rc = interop_nanortc_wait_flag(&nano.dc_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    teardown_pair(&pipe, &nano, &libdatachannel);
}

/* ----------------------------------------------------------------
 * Test: String message from libdatachannel to nanortc
 * ---------------------------------------------------------------- */

TEST(test_interop_dc_string_libdatachannel_to_nanortc)
{
    interop_sig_pipe_t pipe;
    interop_nanortc_peer_t nano;
    interop_libdatachannel_peer_t libdatachannel;

    int rc = setup_connected_pair(&pipe, &nano, &libdatachannel, "echo");
    ASSERT_OK(rc);

    /* Wait for DC open */
    rc = interop_libdatachannel_wait_flag(&libdatachannel.dc_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_nanortc_wait_flag(&nano.dc_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    /* Send string from libdatachannel */
    const char *msg = "hello nanortc";
    int initial_count = atomic_load(&nano.msg_count);
    rc = interop_libdatachannel_send_string(&libdatachannel, msg);
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

    teardown_pair(&pipe, &nano, &libdatachannel);
}

/* ----------------------------------------------------------------
 * Test: String message from nanortc to libdatachannel
 * ---------------------------------------------------------------- */

TEST(test_interop_dc_string_nanortc_to_libdatachannel)
{
    interop_sig_pipe_t pipe;
    interop_nanortc_peer_t nano;
    interop_libdatachannel_peer_t libdatachannel;

    int rc = setup_connected_pair(&pipe, &nano, &libdatachannel, "echo");
    ASSERT_OK(rc);

    /* Wait for DC open */
    rc = interop_libdatachannel_wait_flag(&libdatachannel.dc_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_nanortc_wait_flag(&nano.dc_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    /* Send string from nanortc (need to call from nanortc context) */
    const char *msg = "hello libdatachannel";
    int initial_count = atomic_load(&libdatachannel.msg_count);

    /* Channel handle for stream_id 0 (the DC opened by libdatachannel) */
    nano_channel_t nano_ch;
    nano_ch.rtc = &nano.rtc;
    nano_ch.id = 0;

    rc = nanortc_channel_send_string(&nano_ch, msg);
    ASSERT_OK(rc);

    /* Wait for libdatachannel to receive it */
    uint32_t start = interop_get_millis();
    while (atomic_load(&libdatachannel.msg_count) <= initial_count) {
        ASSERT_TRUE(interop_get_millis() - start < INTEROP_TIMEOUT_MS);
        interop_sleep_ms(10);
    }

    /* Verify */
    pthread_mutex_lock(&libdatachannel.msg_mutex);
    ASSERT_TRUE(libdatachannel.last_msg_is_string);
    ASSERT_EQ(libdatachannel.last_msg_len, strlen(msg));
    ASSERT_TRUE(memcmp(libdatachannel.last_msg, msg, strlen(msg)) == 0);
    pthread_mutex_unlock(&libdatachannel.msg_mutex);

    teardown_pair(&pipe, &nano, &libdatachannel);
}

/* ----------------------------------------------------------------
 * Test: Binary message round-trip
 * ---------------------------------------------------------------- */

TEST(test_interop_dc_binary)
{
    interop_sig_pipe_t pipe;
    interop_nanortc_peer_t nano;
    interop_libdatachannel_peer_t libdatachannel;

    int rc = setup_connected_pair(&pipe, &nano, &libdatachannel, "binary");
    ASSERT_OK(rc);

    rc = interop_libdatachannel_wait_flag(&libdatachannel.dc_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_nanortc_wait_flag(&nano.dc_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    /* Prepare binary payload (256 bytes, pattern fill) */
    uint8_t payload[256];
    for (int i = 0; i < 256; i++) {
        payload[i] = (uint8_t)i;
    }

    int initial_count = atomic_load(&nano.msg_count);
    rc = interop_libdatachannel_send_binary(&libdatachannel, payload, sizeof(payload));
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

    teardown_pair(&pipe, &nano, &libdatachannel);
}

/* ----------------------------------------------------------------
 * Test: Binary message from nanortc to libdatachannel
 * ---------------------------------------------------------------- */

TEST(test_interop_dc_binary_nanortc_to_libdatachannel)
{
    interop_sig_pipe_t pipe;
    interop_nanortc_peer_t nano;
    interop_libdatachannel_peer_t libdatachannel;

    int rc = setup_connected_pair(&pipe, &nano, &libdatachannel, "binary-rev");
    ASSERT_OK(rc);

    rc = interop_libdatachannel_wait_flag(&libdatachannel.dc_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_nanortc_wait_flag(&nano.dc_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    /* Prepare binary payload (256 bytes, pattern fill) */
    uint8_t payload[256];
    for (int i = 0; i < 256; i++) {
        payload[i] = (uint8_t)i;
    }

    int initial_count = atomic_load(&libdatachannel.msg_count);
    nano_channel_t nano_ch;
    nano_ch.rtc = &nano.rtc;
    nano_ch.id = 0;
    rc = nanortc_channel_send(&nano_ch, payload, sizeof(payload));
    ASSERT_OK(rc);

    /* Wait for libdatachannel to receive */
    uint32_t start = interop_get_millis();
    while (atomic_load(&libdatachannel.msg_count) <= initial_count) {
        ASSERT_TRUE(interop_get_millis() - start < INTEROP_TIMEOUT_MS);
        interop_sleep_ms(10);
    }

    /* Verify */
    pthread_mutex_lock(&libdatachannel.msg_mutex);
    ASSERT_FALSE(libdatachannel.last_msg_is_string);
    ASSERT_EQ(libdatachannel.last_msg_len, sizeof(payload));
    ASSERT_TRUE(memcmp(libdatachannel.last_msg, payload, sizeof(payload)) == 0);
    pthread_mutex_unlock(&libdatachannel.msg_mutex);

    teardown_pair(&pipe, &nano, &libdatachannel);
}

/* ----------------------------------------------------------------
 * Test: Large binary message (exercises SCTP fragmentation)
 * ---------------------------------------------------------------- */

TEST(test_interop_dc_large_binary)
{
    interop_sig_pipe_t pipe;
    interop_nanortc_peer_t nano;
    interop_libdatachannel_peer_t libdatachannel;

    int rc = setup_connected_pair(&pipe, &nano, &libdatachannel, "large-bin");
    ASSERT_OK(rc);

    rc = interop_libdatachannel_wait_flag(&libdatachannel.dc_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_nanortc_wait_flag(&nano.dc_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    /* 1000-byte payload (fits in single SCTP DATA chunk, larger than basic 256) */
    uint8_t payload[1000];
    for (int i = 0; i < 1000; i++) {
        payload[i] = (uint8_t)(i & 0xFF);
    }

    int initial_count = atomic_load(&nano.msg_count);
    rc = interop_libdatachannel_send_binary(&libdatachannel, payload, sizeof(payload));
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

    teardown_pair(&pipe, &nano, &libdatachannel);
}

/* ----------------------------------------------------------------
 * Test: Single-byte binary in both directions
 * ---------------------------------------------------------------- */

TEST(test_interop_dc_single_byte)
{
    interop_sig_pipe_t pipe;
    interop_nanortc_peer_t nano;
    interop_libdatachannel_peer_t libdatachannel;

    int rc = setup_connected_pair(&pipe, &nano, &libdatachannel, "single-byte");
    ASSERT_OK(rc);

    rc = interop_libdatachannel_wait_flag(&libdatachannel.dc_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_nanortc_wait_flag(&nano.dc_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    /* libdatachannel -> nanortc: single byte 0x42 */
    uint8_t byte_ldc = 0x42;
    int initial_nano = atomic_load(&nano.msg_count);
    rc = interop_libdatachannel_send_binary(&libdatachannel, &byte_ldc, 1);
    ASSERT_TRUE(rc >= 0);

    uint32_t start = interop_get_millis();
    while (atomic_load(&nano.msg_count) <= initial_nano) {
        ASSERT_TRUE(interop_get_millis() - start < INTEROP_TIMEOUT_MS);
        interop_sleep_ms(10);
    }

    pthread_mutex_lock(&nano.msg_mutex);
    ASSERT_FALSE(nano.last_msg_is_string);
    ASSERT_EQ(nano.last_msg_len, (size_t)1);
    ASSERT_EQ((uint8_t)nano.last_msg[0], (uint8_t)0x42);
    pthread_mutex_unlock(&nano.msg_mutex);

    /* nanortc -> libdatachannel: single byte 0xAB */
    uint8_t byte_nano = 0xAB;
    int initial_ldc = atomic_load(&libdatachannel.msg_count);
    nano_channel_t nano_ch;
    nano_ch.rtc = &nano.rtc;
    nano_ch.id = 0;
    rc = nanortc_channel_send(&nano_ch, &byte_nano, 1);
    ASSERT_OK(rc);

    start = interop_get_millis();
    while (atomic_load(&libdatachannel.msg_count) <= initial_ldc) {
        ASSERT_TRUE(interop_get_millis() - start < INTEROP_TIMEOUT_MS);
        interop_sleep_ms(10);
    }

    pthread_mutex_lock(&libdatachannel.msg_mutex);
    ASSERT_FALSE(libdatachannel.last_msg_is_string);
    ASSERT_EQ(libdatachannel.last_msg_len, (size_t)1);
    ASSERT_EQ((uint8_t)libdatachannel.last_msg[0], (uint8_t)0xAB);
    pthread_mutex_unlock(&libdatachannel.msg_mutex);

    teardown_pair(&pipe, &nano, &libdatachannel);
}

/* ----------------------------------------------------------------
 * Test: Multiple sequential messages
 * ---------------------------------------------------------------- */

#define SEQ_MSG_COUNT 10

TEST(test_interop_dc_sequential_messages)
{
    interop_sig_pipe_t pipe;
    interop_nanortc_peer_t nano;
    interop_libdatachannel_peer_t libdatachannel;

    int rc = setup_connected_pair(&pipe, &nano, &libdatachannel, "sequential");
    ASSERT_OK(rc);

    rc = interop_libdatachannel_wait_flag(&libdatachannel.dc_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_nanortc_wait_flag(&nano.dc_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    int initial_count = atomic_load(&nano.msg_count);

    /* Send 10 string messages from libdatachannel, wait for each */
    for (int i = 0; i < SEQ_MSG_COUNT; i++) {
        char msg[32];
        int msg_len = snprintf(msg, sizeof(msg), "msg-%d", i);

        rc = interop_libdatachannel_send_string(&libdatachannel, msg);
        ASSERT_TRUE(rc >= 0);

        /* Wait for nanortc to receive this message */
        int expected = initial_count + i + 1;
        uint32_t start = interop_get_millis();
        while (atomic_load(&nano.msg_count) < expected) {
            ASSERT_TRUE(interop_get_millis() - start < INTEROP_TIMEOUT_MS);
            interop_sleep_ms(10);
        }

        /* Verify this message */
        pthread_mutex_lock(&nano.msg_mutex);
        ASSERT_TRUE(nano.last_msg_is_string);
        ASSERT_EQ(nano.last_msg_len, (size_t)msg_len);
        ASSERT_TRUE(memcmp(nano.last_msg, msg, (size_t)msg_len) == 0);
        pthread_mutex_unlock(&nano.msg_mutex);
    }

    /* Final count check */
    ASSERT_EQ(atomic_load(&nano.msg_count), initial_count + SEQ_MSG_COUNT);

    teardown_pair(&pipe, &nano, &libdatachannel);
}

/* ----------------------------------------------------------------
 * Test: Bidirectional simultaneous messages
 * ---------------------------------------------------------------- */

TEST(test_interop_dc_bidirectional)
{
    interop_sig_pipe_t pipe;
    interop_nanortc_peer_t nano;
    interop_libdatachannel_peer_t libdatachannel;

    int rc = setup_connected_pair(&pipe, &nano, &libdatachannel, "bidir");
    ASSERT_OK(rc);

    rc = interop_libdatachannel_wait_flag(&libdatachannel.dc_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_nanortc_wait_flag(&nano.dc_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    int initial_nano = atomic_load(&nano.msg_count);
    int initial_ldc = atomic_load(&libdatachannel.msg_count);

    /* Both sides send before either checks for receipt */
    const char *from_nano = "from-nano";
    const char *from_ldc = "from-libdc";

    nano_channel_t nano_ch;
    nano_ch.rtc = &nano.rtc;
    nano_ch.id = 0;
    rc = nanortc_channel_send_string(&nano_ch, from_nano);
    ASSERT_OK(rc);
    rc = interop_libdatachannel_send_string(&libdatachannel, from_ldc);
    ASSERT_TRUE(rc >= 0);

    /* Wait for both sides to receive */
    uint32_t start = interop_get_millis();
    while (atomic_load(&nano.msg_count) <= initial_nano ||
           atomic_load(&libdatachannel.msg_count) <= initial_ldc) {
        ASSERT_TRUE(interop_get_millis() - start < INTEROP_TIMEOUT_MS);
        interop_sleep_ms(10);
    }

    /* Verify nanortc received from libdatachannel */
    pthread_mutex_lock(&nano.msg_mutex);
    ASSERT_TRUE(nano.last_msg_is_string);
    ASSERT_EQ(nano.last_msg_len, strlen(from_ldc));
    ASSERT_TRUE(memcmp(nano.last_msg, from_ldc, strlen(from_ldc)) == 0);
    pthread_mutex_unlock(&nano.msg_mutex);

    /* Verify libdatachannel received from nanortc */
    pthread_mutex_lock(&libdatachannel.msg_mutex);
    ASSERT_TRUE(libdatachannel.last_msg_is_string);
    ASSERT_EQ(libdatachannel.last_msg_len, strlen(from_nano));
    ASSERT_TRUE(memcmp(libdatachannel.last_msg, from_nano, strlen(from_nano)) == 0);
    pthread_mutex_unlock(&libdatachannel.msg_mutex);

    teardown_pair(&pipe, &nano, &libdatachannel);
}

/* ----------------------------------------------------------------
 * Test: Echo round-trip (libdc -> nano -> libdc)
 * ---------------------------------------------------------------- */

TEST(test_interop_dc_echo_roundtrip)
{
    interop_sig_pipe_t pipe;
    interop_nanortc_peer_t nano;
    interop_libdatachannel_peer_t libdatachannel;

    int rc = setup_connected_pair(&pipe, &nano, &libdatachannel, "echo-rt");
    ASSERT_OK(rc);

    rc = interop_libdatachannel_wait_flag(&libdatachannel.dc_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_nanortc_wait_flag(&nano.dc_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    /* Step 1: libdatachannel sends request to nanortc */
    const char *request = "echo-request";
    int initial_nano = atomic_load(&nano.msg_count);
    rc = interop_libdatachannel_send_string(&libdatachannel, request);
    ASSERT_TRUE(rc >= 0);

    uint32_t start = interop_get_millis();
    while (atomic_load(&nano.msg_count) <= initial_nano) {
        ASSERT_TRUE(interop_get_millis() - start < INTEROP_TIMEOUT_MS);
        interop_sleep_ms(10);
    }

    /* Verify nanortc received the request */
    pthread_mutex_lock(&nano.msg_mutex);
    ASSERT_TRUE(nano.last_msg_is_string);
    ASSERT_EQ(nano.last_msg_len, strlen(request));
    ASSERT_TRUE(memcmp(nano.last_msg, request, strlen(request)) == 0);
    pthread_mutex_unlock(&nano.msg_mutex);

    /* Step 2: nanortc echoes back a reply */
    const char *reply = "echo-reply";
    int initial_ldc = atomic_load(&libdatachannel.msg_count);
    nano_channel_t nano_ch;
    nano_ch.rtc = &nano.rtc;
    nano_ch.id = 0;
    rc = nanortc_channel_send_string(&nano_ch, reply);
    ASSERT_OK(rc);

    start = interop_get_millis();
    while (atomic_load(&libdatachannel.msg_count) <= initial_ldc) {
        ASSERT_TRUE(interop_get_millis() - start < INTEROP_TIMEOUT_MS);
        interop_sleep_ms(10);
    }

    /* Verify libdatachannel received the reply */
    pthread_mutex_lock(&libdatachannel.msg_mutex);
    ASSERT_TRUE(libdatachannel.last_msg_is_string);
    ASSERT_EQ(libdatachannel.last_msg_len, strlen(reply));
    ASSERT_TRUE(memcmp(libdatachannel.last_msg, reply, strlen(reply)) == 0);
    pthread_mutex_unlock(&libdatachannel.msg_mutex);

    teardown_pair(&pipe, &nano, &libdatachannel);
}

/* ----------------------------------------------------------------
 * Test runner
 * ---------------------------------------------------------------- */

TEST_MAIN_BEGIN("interop-datachannel")
RUN(test_interop_handshake);
RUN(test_interop_dc_open);
RUN(test_interop_dc_string_libdatachannel_to_nanortc);
RUN(test_interop_dc_string_nanortc_to_libdatachannel);
RUN(test_interop_dc_binary);
RUN(test_interop_dc_binary_nanortc_to_libdatachannel);
RUN(test_interop_dc_large_binary);
RUN(test_interop_dc_single_byte);
RUN(test_interop_dc_sequential_messages);
RUN(test_interop_dc_bidirectional);
RUN(test_interop_dc_echo_roundtrip);
TEST_MAIN_END
