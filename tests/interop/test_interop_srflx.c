/*
 * nanortc interop tests — server-reflexive (srflx) candidate, end-to-end
 *
 * Test topology:
 *   libdatachannel (offerer/controlling, host candidate)
 *      <--localhost UDP-->
 *   nanortc (answerer/controlled, srflx-only)
 *
 * A small in-process fake STUN server (interop_fake_stun) returns a
 * configurable XOR-MAPPED-ADDRESS so srflx discovery is deterministic and
 * does not depend on internet connectivity. The fake STUN's mapped address
 * is set to the same loopback IP and port as nanortc's UDP socket — at the
 * wire level srflx == host, but ICE-side the candidate is labeled SRFLX,
 * exercising:
 *   - srflx insertion into ice.local_candidates[] (NANORTC_FEATURE_ICE_SRFLX)
 *   - ICE_SRFLX_PRIORITY in outgoing connectivity check PRIORITY attr
 *   - selected_local_type == NANORTC_ICE_CAND_SRFLX after USE-CANDIDATE
 *   - DataChannel data flowing over the srflx-selected pair
 *
 * The host candidate is intentionally suppressed (srflx_only mode), so libdc
 * must use the srflx candidate to reach nanortc.
 *
 * SPDX-License-Identifier: MIT
 */

#include "interop_common.h"
#include "interop_fake_stun.h"
#include "interop_libdatachannel_peer.h"
#include "interop_nanortc_peer.h"
#include "nano_test.h"
#include "nanortc.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

/* Use a port range that doesn't collide with the other interop tests. */
#define INTEROP_SRFLX_PORT_BASE 19400
static uint16_t next_port = INTEROP_SRFLX_PORT_BASE;
static uint16_t alloc_port(void)
{
    return next_port++;
}

TEST(test_interop_srflx_handshake_and_data)
{
    interop_sig_pipe_t pipe;
    interop_nanortc_peer_t nano;
    interop_libdatachannel_peer_t libdc;
    interop_fake_stun_t stun;

    uint16_t nano_port = alloc_port();

    /* Fake STUN: mapped address = (127.0.0.1, nano_port). srflx therefore
     * resolves to the same wire endpoint as nanortc's host socket, so libdc
     * sending to srflx actually lands on nanortc. The candidate is still
     * labeled SRFLX in the SDP, exercising the new local-candidate path. */
    uint8_t loopback[16] = {0};
    inet_pton(AF_INET, "127.0.0.1", loopback);
    int rc = interop_fake_stun_start(&stun, /* bind_port */ 0, loopback, nano_port, 4);
    TEST_ASSERT_EQUAL_INT(0, rc);
    fprintf(stderr, "[srflx-test] fake STUN listening on 127.0.0.1:%u\n", stun.bind_port);

    /* Build the iceServers config pointing nanortc at our fake STUN. */
    char stun_url_buf[64];
    snprintf(stun_url_buf, sizeof(stun_url_buf), "stun:127.0.0.1:%u", stun.bind_port);
    const char *stun_url = stun_url_buf;
    nanortc_ice_server_t servers[1] = {{
        .urls = &stun_url,
        .url_count = 1,
        .username = NULL,
        .credential = NULL,
    }};

    char resolve_scratch[256];
    interop_nanortc_ice_config_t ice_cfg = {
        .ice_servers = servers,
        .ice_server_count = 1,
        .resolve_scratch = resolve_scratch,
        .resolve_scratch_size = sizeof(resolve_scratch),
        .relay_only = 0,
        .srflx_only = 1,
    };

    rc = interop_sig_create(&pipe);
    TEST_ASSERT_EQUAL_INT(0, rc);

    rc = interop_nanortc_start(&nano, pipe.fd[0], nano_port, &ice_cfg);
    TEST_ASSERT_EQUAL_INT(0, rc);

    rc = interop_libdatachannel_start(&libdc, pipe.fd[1], "srflx", nano_port);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* Wait for both sides to fully connect (DC open). */
    rc = interop_libdatachannel_wait_flag(&libdc.dc_open, INTEROP_TIMEOUT_MS);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "libdatachannel DC did not open");
    rc = interop_nanortc_wait_flag(&nano.dc_open, INTEROP_TIMEOUT_MS);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "nanortc DC did not open");

    /* Selected pair must be SRFLX on the local (nanortc) side. This is the
     * load-bearing assertion: without the srflx-into-local-candidates fix,
     * ice.local_candidates[] would be empty (srflx_only) and the controlled
     * side would fall back to selected_local_idx=0 (host) — i.e. type=HOST.
     * With the fix, the only local candidate is SRFLX, dst→idx resolves to
     * it, and selected_local_type is recorded as SRFLX. */
    uint8_t local_type = __atomic_load_n(&nano.rtc.ice.selected_local_type, __ATOMIC_RELAXED);
    TEST_ASSERT_EQUAL_INT_MESSAGE(NANORTC_ICE_CAND_SRFLX, local_type,
                                  "nanortc selected_local_type != SRFLX");

    /* Round-trip a DataChannel message both ways to confirm SCTP works
     * over the srflx-selected pair. */
    const char *msg_to_nano = "hello-srflx";
    int initial_nano = atomic_load(&nano.msg_count);
    rc = interop_libdatachannel_send_string(&libdc, msg_to_nano);
    TEST_ASSERT_TRUE(rc >= 0);

    uint32_t start = interop_get_millis();
    while (atomic_load(&nano.msg_count) <= initial_nano) {
        TEST_ASSERT_TRUE(interop_get_millis() - start < INTEROP_TIMEOUT_MS);
        interop_sleep_ms(10);
    }
    pthread_mutex_lock(&nano.msg_mutex);
    TEST_ASSERT_TRUE(nano.last_msg_is_string);
    TEST_ASSERT_EQUAL_INT(strlen(msg_to_nano), nano.last_msg_len);
    TEST_ASSERT_TRUE(memcmp(nano.last_msg, msg_to_nano, strlen(msg_to_nano)) == 0);
    pthread_mutex_unlock(&nano.msg_mutex);

    const char *msg_to_libdc = "world-srflx";
    int initial_libdc = atomic_load(&libdc.msg_count);
    rc = nanortc_datachannel_send_string(&nano.rtc, 0, msg_to_libdc);
    TEST_ASSERT_EQUAL_INT(NANORTC_OK, rc);

    start = interop_get_millis();
    while (atomic_load(&libdc.msg_count) <= initial_libdc) {
        TEST_ASSERT_TRUE(interop_get_millis() - start < INTEROP_TIMEOUT_MS);
        interop_sleep_ms(10);
    }
    pthread_mutex_lock(&libdc.msg_mutex);
    TEST_ASSERT_TRUE(libdc.last_msg_is_string);
    TEST_ASSERT_EQUAL_INT(strlen(msg_to_libdc), libdc.last_msg_len);
    TEST_ASSERT_TRUE(memcmp(libdc.last_msg, msg_to_libdc, strlen(msg_to_libdc)) == 0);
    pthread_mutex_unlock(&libdc.msg_mutex);

    interop_libdatachannel_stop(&libdc);
    interop_nanortc_stop(&nano);
    interop_sig_destroy(&pipe);
    interop_fake_stun_stop(&stun);
}

TEST_MAIN_BEGIN("interop-srflx")
RUN(test_interop_srflx_handshake_and_data);
TEST_MAIN_END
