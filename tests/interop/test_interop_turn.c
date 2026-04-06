/*
 * nanortc interop tests — TURN allocation + ICE interoperability
 *
 * Test topology:
 *   libdatachannel (offerer, host-only) <--localhost--> nanortc (answerer, STUN+TURN)
 *
 * nanortc is configured with STUN + TURN servers and performs TURN allocation
 * against a real TURN server. Both peers connect via host candidates (same
 * machine), while nanortc exercises the full TURN allocation path in the
 * background (Allocate → 401 → authenticated Allocate → relay candidate).
 *
 * Defaults to PeerJS public TURN servers (no config needed).
 * Override via environment variables:
 *   NANORTC_TURN_URL  — e.g. "turn:eu-0.turn.peerjs.com:3478"
 *   NANORTC_TURN_USER — e.g. "peerjs"
 *   NANORTC_TURN_PASS — e.g. "peerjsp"
 *   NANORTC_STUN_URL  — e.g. "stun:stun.l.google.com:19302"
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_test.h"
#include "interop_common.h"
#include "interop_nanortc_peer.h"
#include "interop_libdatachannel_peer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------------------------------------------
 * ICE server config — PeerJS defaults with env var override
 * ---------------------------------------------------------------- */

static const char *turn_url;
static const char *turn_user;
static const char *turn_pass;
static const char *stun_url;

static void load_ice_config(void)
{
    const char *env;

    env = getenv("NANORTC_STUN_URL");
    stun_url = (env && env[0]) ? env : "stun:stun.l.google.com:19302";

    env = getenv("NANORTC_TURN_URL");
    turn_url = (env && env[0]) ? env : "turn:eu-0.turn.peerjs.com:3478";

    env = getenv("NANORTC_TURN_USER");
    turn_user = (env && env[0]) ? env : "peerjs";

    env = getenv("NANORTC_TURN_PASS");
    turn_pass = (env && env[0]) ? env : "peerjsp";
}

/* ----------------------------------------------------------------
 * Port allocation
 * ---------------------------------------------------------------- */

static uint16_t next_turn_port = INTEROP_TURN_PORT_BASE;

static uint16_t alloc_turn_port(void)
{
    return next_turn_port++;
}

/* ----------------------------------------------------------------
 * Per-test ICE config storage (avoids stale pointers between tests)
 * ---------------------------------------------------------------- */

#define MAX_TURN_TESTS 3

static const char *stun_url_ptrs[MAX_TURN_TESTS];
static const char *turn_url_ptrs[MAX_TURN_TESTS];
static nanortc_ice_server_t ice_servers_store[MAX_TURN_TESTS][2];
static char resolve_scratches[MAX_TURN_TESTS][1024];
static int setup_idx;

/* ----------------------------------------------------------------
 * TURN-aware connected pair setup
 * ---------------------------------------------------------------- */

static int setup_turn_pair(interop_sig_pipe_t *pipe, interop_nanortc_peer_t *nano,
                           interop_libdatachannel_peer_t *ldc, const char *dc_label)
{
    uint16_t port = alloc_turn_port();
    int idx = setup_idx++;

    if (interop_sig_create(pipe) != 0) {
        fprintf(stderr, "[test] Failed to create signaling pipe\n");
        return -1;
    }

    /* nanortc ICE server config */
    stun_url_ptrs[idx] = stun_url;
    turn_url_ptrs[idx] = turn_url;

    ice_servers_store[idx][0] = (nanortc_ice_server_t){.urls = &stun_url_ptrs[idx], .url_count = 1};
    ice_servers_store[idx][1] = (nanortc_ice_server_t){.urls = &turn_url_ptrs[idx],
                                                       .url_count = 1,
                                                       .username = turn_user,
                                                       .credential = turn_pass};

    interop_nanortc_ice_config_t nano_ice = {
        .ice_servers = ice_servers_store[idx],
        .ice_server_count = 2,
        .resolve_scratch = resolve_scratches[idx],
        .resolve_scratch_size = sizeof(resolve_scratches[idx]),
    };

    fprintf(stderr, "[test] STUN: %s\n", stun_url);
    fprintf(stderr, "[test] TURN (nanortc): %s user=%s\n", turn_url, turn_user);

    /* Start nanortc with ICE servers (TURN warmup → signaling → event loop) */
    if (interop_nanortc_start(nano, pipe->fd[0], port, &nano_ice) != 0) {
        fprintf(stderr, "[test] Failed to start nanortc peer\n");
        interop_sig_destroy(pipe);
        return -1;
    }

    /* Start libdatachannel host-only — both connect via host candidates.
     * nanortc exercises TURN allocation in the background. */
    if (interop_libdatachannel_start(ldc, pipe->fd[1], dc_label, port) != 0) {
        fprintf(stderr, "[test] Failed to start libdatachannel peer\n");
        interop_nanortc_stop(nano);
        interop_sig_destroy(pipe);
        return -1;
    }

    return 0;
}

static void teardown_turn_pair(interop_sig_pipe_t *pipe, interop_nanortc_peer_t *nano,
                               interop_libdatachannel_peer_t *ldc)
{
    interop_libdatachannel_stop(ldc);
    interop_nanortc_stop(nano);
    interop_sig_destroy(pipe);
}

/* ----------------------------------------------------------------
 * Test: TURN handshake — ICE + DTLS + SCTP with TURN configured
 * ---------------------------------------------------------------- */

TEST(test_interop_turn_handshake)
{
    interop_sig_pipe_t pipe;
    interop_nanortc_peer_t nano;
    interop_libdatachannel_peer_t ldc;

    int rc = setup_turn_pair(&pipe, &nano, &ldc, "turn-test");
    ASSERT_OK(rc);

    rc = interop_libdatachannel_wait_flag(&ldc.connected, INTEROP_TURN_TIMEOUT_MS);
    ASSERT_OK(rc);

    rc = interop_nanortc_wait_flag(&nano.sctp_connected, INTEROP_TURN_TIMEOUT_MS);
    ASSERT_OK(rc);

    ASSERT_TRUE(atomic_load(&nano.ice_connected));
    ASSERT_TRUE(atomic_load(&nano.dtls_connected));
    ASSERT_TRUE(atomic_load(&nano.sctp_connected));

    teardown_turn_pair(&pipe, &nano, &ldc);
}

/* ----------------------------------------------------------------
 * Test: String message exchange with TURN configured
 * ---------------------------------------------------------------- */

TEST(test_interop_turn_dc_string)
{
    interop_sig_pipe_t pipe;
    interop_nanortc_peer_t nano;
    interop_libdatachannel_peer_t ldc;

    int rc = setup_turn_pair(&pipe, &nano, &ldc, "turn-str");
    ASSERT_OK(rc);

    rc = interop_libdatachannel_wait_flag(&ldc.dc_open, INTEROP_TURN_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_nanortc_wait_flag(&nano.dc_open, INTEROP_TURN_TIMEOUT_MS);
    ASSERT_OK(rc);

    /* libdatachannel -> nanortc */
    const char *msg_to_nano = "hello nanortc via TURN";
    int initial_count = atomic_load(&nano.msg_count);
    rc = interop_libdatachannel_send_string(&ldc, msg_to_nano);
    ASSERT_TRUE(rc >= 0);

    uint32_t start = interop_get_millis();
    while (atomic_load(&nano.msg_count) <= initial_count) {
        ASSERT_TRUE(interop_get_millis() - start < INTEROP_TURN_TIMEOUT_MS);
        interop_sleep_ms(10);
    }

    pthread_mutex_lock(&nano.msg_mutex);
    ASSERT_TRUE(nano.last_msg_is_string);
    ASSERT_EQ(nano.last_msg_len, strlen(msg_to_nano));
    ASSERT_TRUE(memcmp(nano.last_msg, msg_to_nano, strlen(msg_to_nano)) == 0);
    pthread_mutex_unlock(&nano.msg_mutex);

    /* nanortc -> libdatachannel */
    const char *msg_to_ldc = "hello libdc via TURN";
    int initial_ldc = atomic_load(&ldc.msg_count);
    rc = nanortc_datachannel_send_string(&nano.rtc, 0, msg_to_ldc);
    ASSERT_OK(rc);

    start = interop_get_millis();
    while (atomic_load(&ldc.msg_count) <= initial_ldc) {
        ASSERT_TRUE(interop_get_millis() - start < INTEROP_TURN_TIMEOUT_MS);
        interop_sleep_ms(10);
    }

    pthread_mutex_lock(&ldc.msg_mutex);
    ASSERT_TRUE(ldc.last_msg_is_string);
    ASSERT_EQ(ldc.last_msg_len, strlen(msg_to_ldc));
    ASSERT_TRUE(memcmp(ldc.last_msg, msg_to_ldc, strlen(msg_to_ldc)) == 0);
    pthread_mutex_unlock(&ldc.msg_mutex);

    teardown_turn_pair(&pipe, &nano, &ldc);
}

/* ----------------------------------------------------------------
 * Test: Echo round-trip with TURN configured (libdc -> nano -> libdc)
 * ---------------------------------------------------------------- */

TEST(test_interop_turn_dc_echo_roundtrip)
{
    interop_sig_pipe_t pipe;
    interop_nanortc_peer_t nano;
    interop_libdatachannel_peer_t ldc;

    int rc = setup_turn_pair(&pipe, &nano, &ldc, "turn-echo");
    ASSERT_OK(rc);

    rc = interop_libdatachannel_wait_flag(&ldc.dc_open, INTEROP_TURN_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_nanortc_wait_flag(&nano.dc_open, INTEROP_TURN_TIMEOUT_MS);
    ASSERT_OK(rc);

    /* Step 1: libdatachannel sends request */
    const char *request = "echo-via-turn";
    int initial_nano = atomic_load(&nano.msg_count);
    rc = interop_libdatachannel_send_string(&ldc, request);
    ASSERT_TRUE(rc >= 0);

    uint32_t start = interop_get_millis();
    while (atomic_load(&nano.msg_count) <= initial_nano) {
        ASSERT_TRUE(interop_get_millis() - start < INTEROP_TURN_TIMEOUT_MS);
        interop_sleep_ms(10);
    }

    pthread_mutex_lock(&nano.msg_mutex);
    ASSERT_TRUE(nano.last_msg_is_string);
    ASSERT_EQ(nano.last_msg_len, strlen(request));
    ASSERT_TRUE(memcmp(nano.last_msg, request, strlen(request)) == 0);
    pthread_mutex_unlock(&nano.msg_mutex);

    /* Step 2: nanortc echoes back */
    const char *reply = "echo-reply-via-turn";
    int initial_ldc = atomic_load(&ldc.msg_count);
    rc = nanortc_datachannel_send_string(&nano.rtc, 0, reply);
    ASSERT_OK(rc);

    start = interop_get_millis();
    while (atomic_load(&ldc.msg_count) <= initial_ldc) {
        ASSERT_TRUE(interop_get_millis() - start < INTEROP_TURN_TIMEOUT_MS);
        interop_sleep_ms(10);
    }

    pthread_mutex_lock(&ldc.msg_mutex);
    ASSERT_TRUE(ldc.last_msg_is_string);
    ASSERT_EQ(ldc.last_msg_len, strlen(reply));
    ASSERT_TRUE(memcmp(ldc.last_msg, reply, strlen(reply)) == 0);
    pthread_mutex_unlock(&ldc.msg_mutex);

    teardown_turn_pair(&pipe, &nano, &ldc);
}

/* ----------------------------------------------------------------
 * Test runner
 * ---------------------------------------------------------------- */

int main(void)
{
    load_ice_config();
    printf("TURN interop test: STUN=%s TURN=%s user=%s\n", stun_url, turn_url, turn_user);

    UNITY_BEGIN();
    RUN_TEST(test_interop_turn_handshake);
    RUN_TEST(test_interop_turn_dc_string);
    RUN_TEST(test_interop_turn_dc_echo_roundtrip);
    return UNITY_END();
}
