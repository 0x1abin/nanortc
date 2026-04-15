/*
 * nanortc interop tests — TURN relay-only data path
 *
 * These tests *force* the libdatachannel side into relay-only mode by
 * setting rtcConfiguration.iceTransportPolicy = RTC_TRANSPORT_POLICY_RELAY.
 * libdatachannel then advertises only its relay candidate, so nanortc has
 * no host/srflx pair available — the only way the peers can communicate is
 * via the configured TURN server.
 *
 * Unlike test_interop_turn.c (which connects via host candidates and runs
 * TURN allocation as a no-op background warmup), every byte exchanged here
 * traverses the TURN server and exercises Send Indication / Data Indication
 * or ChannelData framing on both sides.
 *
 * Default TURN server: turn:127.0.0.1:3478 (local coturn from
 * scripts/start-test-turn.sh). Override via environment:
 *   NANORTC_TURN_URL  e.g. "turn:turn-cn.uipcat.com:3478"
 *   NANORTC_TURN_USER e.g. "alice"
 *   NANORTC_TURN_PASS e.g. "secret"
 *   NANORTC_STUN_URL  e.g. "stun:stun.l.google.com:19302"
 *
 * If the configured TURN server is unreachable, all tests are skipped
 * (printed message + return 0) so CI without a TURN server stays green.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_test.h"
#include "interop_common.h"
#include "interop_nanortc_peer.h"
#include "interop_libdatachannel_peer.h"
#include "nano_ice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

/* ----------------------------------------------------------------
 * ICE server config — local coturn defaults with env var override
 * ---------------------------------------------------------------- */

static const char *turn_url;
static const char *turn_user;
static const char *turn_pass;
static const char *stun_url;
static int turn_server_reachable = 0;

static void load_ice_config(void)
{
    const char *env;

    env = getenv("NANORTC_STUN_URL");
    stun_url = (env && env[0]) ? env : "stun:stun.l.google.com:19302";

    env = getenv("NANORTC_TURN_URL");
    turn_url = (env && env[0]) ? env : "turn:127.0.0.1:3478";

    env = getenv("NANORTC_TURN_USER");
    turn_user = (env && env[0]) ? env : "testuser";

    env = getenv("NANORTC_TURN_PASS");
    turn_pass = (env && env[0]) ? env : "testpass";
}

/* Probe whether the configured TURN server is actually live by sending a
 * real STUN Binding Request and waiting briefly for a response. Coturn
 * answers Binding requests without authentication, so a successful Binding
 * round-trip proves the server is up. We can't just rely on sendto() to
 * 127.0.0.1: loopback delivers packets to a non-existent listener without
 * error, so the kernel return code is meaningless on localhost.
 * Returns 1 if reachable (got valid Binding response), 0 otherwise. */
static int probe_turn_server(void)
{
    const char *p = turn_url;
    if (strncmp(p, "turn:", 5) != 0 && strncmp(p, "turns:", 6) != 0) {
        return 0;
    }
    p += (p[4] == ':') ? 5 : 6;

    char host[256];
    const char *colon = strrchr(p, ':');
    if (!colon || (size_t)(colon - p) >= sizeof(host)) {
        return 0;
    }
    memcpy(host, p, (size_t)(colon - p));
    host[colon - p] = '\0';
    int port = atoi(colon + 1);
    if (port <= 0 || port > 65535) {
        return 0;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
        return 0;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        freeaddrinfo(res);
        return 0;
    }
    struct sockaddr_in sa;
    memcpy(&sa, res->ai_addr, sizeof(sa));
    sa.sin_port = htons((uint16_t)port);
    freeaddrinfo(res);

    /* Build a 20-byte STUN Binding Request: type=0x0001, length=0,
     * magic cookie 0x2112A442, random 12-byte transaction id. RFC 8489 §5. */
    uint8_t req[20];
    req[0] = 0x00;
    req[1] = 0x01; /* Binding Request */
    req[2] = 0x00;
    req[3] = 0x00; /* length */
    req[4] = 0x21;
    req[5] = 0x12;
    req[6] = 0xA4;
    req[7] = 0x42; /* cookie */
    for (int i = 8; i < 20; i++) {
        req[i] = (uint8_t)(0xC0 + i);
    }

    int reachable = 0;
    int attempts;
    for (attempts = 0; attempts < 3 && !reachable; attempts++) {
        if (sendto(fd, req, sizeof(req), 0, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
            break;
        }
        struct timeval tv = {.tv_sec = 0, .tv_usec = 250000}; /* 250 ms */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        int rs = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (rs > 0) {
            uint8_t resp[256];
            ssize_t n = recv(fd, resp, sizeof(resp), 0);
            /* Successful response: STUN Binding Success (0x0101) with the
             * same magic cookie + transaction id as our request. */
            if (n >= 20 && resp[0] == 0x01 && resp[1] == 0x01 && memcmp(resp + 4, req + 4, 16) == 0) {
                reachable = 1;
            }
        }
    }

    close(fd);
    return reachable;
}

/* ----------------------------------------------------------------
 * Port allocation — separate range from regular TURN tests
 * ---------------------------------------------------------------- */

static uint16_t next_relay_port = INTEROP_TURN_PORT_BASE + 50;

static uint16_t alloc_relay_port(void)
{
    return next_relay_port++;
}

/* ----------------------------------------------------------------
 * Per-test ICE config storage
 * ---------------------------------------------------------------- */

#define MAX_RELAY_TESTS 6

static const char *stun_url_ptrs[MAX_RELAY_TESTS];
static const char *turn_url_ptrs[MAX_RELAY_TESTS];
static nanortc_ice_server_t ice_servers_store[MAX_RELAY_TESTS][2];
static char resolve_scratches[MAX_RELAY_TESTS][1024];
static int setup_idx;

/* ----------------------------------------------------------------
 * relay-only pair setup
 *
 * Only libdatachannel is forced relay-only (via iceTransportPolicy) and
 * given STUN/TURN. nanortc runs in plain host-only mode without any TURN
 * configuration of its own.
 *
 * This is intentional: in TURN, the server unwraps Send Indication /
 * ChannelData on the peer-bound side and forwards the raw UDP payload to
 * the peer. From nanortc's perspective, coturn looks like a regular peer
 * sitting at <turn-server-host>:<some-ephemeral-port>. nanortc can talk
 * to it without knowing TURN exists, while every byte still traverses
 * coturn from libdc's side. Configuring nanortc with TURN as well would
 * just add a second, independent allocation path with its own warmup
 * delays and is irrelevant to "is libdc actually relayed".
 * ---------------------------------------------------------------- */

static int setup_relay_pair(interop_sig_pipe_t *pipe, interop_nanortc_peer_t *nano,
                            interop_libdatachannel_peer_t *ldc, const char *dc_label)
{
    uint16_t port = alloc_relay_port();
    (void)setup_idx; /* not used in this mode */

    if (interop_sig_create(pipe) != 0) {
        fprintf(stderr, "[test] Failed to create signaling pipe\n");
        return -1;
    }

    fprintf(stderr, "[test] STUN: %s\n", stun_url);
    fprintf(stderr, "[test] TURN: %s user=%s\n", turn_url, turn_user);

    /* nanortc: localhost-only (no ICE servers). */
    if (interop_nanortc_start(nano, pipe->fd[0], port, NULL) != 0) {
        fprintf(stderr, "[test] Failed to start nanortc peer\n");
        interop_sig_destroy(pipe);
        return -1;
    }

    /* libdatachannel: relay-only, with STUN+TURN. */
    interop_libdatachannel_ice_config_t ldc_ice = {
        .relay_only = 1,
        .stun_url = stun_url,
        .turn_url = turn_url,
        .turn_user = turn_user,
        .turn_pass = turn_pass,
    };

    if (interop_libdatachannel_start_ex(ldc, pipe->fd[1], dc_label, port, &ldc_ice) != 0) {
        fprintf(stderr, "[test] Failed to start libdatachannel peer\n");
        interop_nanortc_stop(nano);
        interop_sig_destroy(pipe);
        return -1;
    }

    return 0;
}

static void teardown_relay_pair(interop_sig_pipe_t *pipe, interop_nanortc_peer_t *nano,
                                interop_libdatachannel_peer_t *ldc)
{
    interop_libdatachannel_stop(ldc);
    interop_nanortc_stop(nano);
    interop_sig_destroy(pipe);
}

/* Assert that the data path actually traverses the TURN server.
 *
 * libdatachannel is configured with iceTransportPolicy=RELAY, so it will
 * only form ICE pairs that involve its own TURN-allocated relay candidate.
 * If the connection establishes and DC traffic flows, the data path *must*
 * be going through coturn — libdc has no other socket to talk on.
 *
 * We verify two observable invariants on nanortc's side:
 *   (1) libdc advertised a relay candidate via SDP/trickle (proving the
 *       relay-only configuration actually took effect at libdc).
 *   (2) nanortc received and stored that candidate in remote_candidates[].
 *
 * Combined with the test's prior wait-for-DC-open, this is a sufficient
 * proof that data crosses the TURN server.
 *
 * We deliberately do NOT assert nanortc's own selected_type == RELAY. That
 * would require nanortc itself to allocate through TURN, which is an
 * orthogonal path. We also do NOT inspect the selected remote port: coturn
 * relays peer-bound data over an *ephemeral* outbound socket whose port is
 * not knowable from the client side, so any port comparison here would
 * silently break with a different TURN server. */
static void assert_relay_path(const interop_nanortc_peer_t *nano)
{
    const nano_ice_t *ice = &nano->rtc.ice;

    int relay_seen = 0;
    for (uint8_t i = 0; i < ice->remote_candidate_count; i++) {
        if (ice->remote_candidates[i].type == NANORTC_ICE_CAND_RELAY) {
            relay_seen = 1;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(relay_seen,
                             "libdc did not advertise a relay candidate "
                             "(iceTransportPolicy=relay not effective?)");
}

/* ----------------------------------------------------------------
 * Skip-on-no-server guard — used at the top of each test
 * ---------------------------------------------------------------- */

#define SKIP_IF_NO_TURN()                                                                          \
    do {                                                                                           \
        if (!turn_server_reachable) {                                                              \
            fprintf(stderr, "[test] SKIP: TURN server unreachable (%s)\n", turn_url);              \
            return;                                                                                \
        }                                                                                          \
    } while (0)

/* ----------------------------------------------------------------
 * Test 1: handshake — ICE+DTLS+SCTP via relay
 * ---------------------------------------------------------------- */

TEST(test_relay_only_handshake)
{
    SKIP_IF_NO_TURN();

    interop_sig_pipe_t pipe;
    interop_nanortc_peer_t nano;
    interop_libdatachannel_peer_t ldc;

    int rc = setup_relay_pair(&pipe, &nano, &ldc, "relay-handshake");
    ASSERT_OK(rc);

    rc = interop_libdatachannel_wait_flag(&ldc.connected, INTEROP_TURN_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_nanortc_wait_flag(&nano.sctp_connected, INTEROP_TURN_TIMEOUT_MS);
    ASSERT_OK(rc);

    ASSERT_TRUE(atomic_load(&nano.ice_connected));
    ASSERT_TRUE(atomic_load(&nano.dtls_connected));
    ASSERT_TRUE(atomic_load(&nano.sctp_connected));
    assert_relay_path(&nano);

    teardown_relay_pair(&pipe, &nano, &ldc);
}

/* ----------------------------------------------------------------
 * Test 2: bidirectional DC strings — every byte traverses TURN
 * ---------------------------------------------------------------- */

TEST(test_relay_only_dc_string_bidirectional)
{
    SKIP_IF_NO_TURN();

    interop_sig_pipe_t pipe;
    interop_nanortc_peer_t nano;
    interop_libdatachannel_peer_t ldc;

    int rc = setup_relay_pair(&pipe, &nano, &ldc, "relay-string");
    ASSERT_OK(rc);

    rc = interop_libdatachannel_wait_flag(&ldc.dc_open, INTEROP_TURN_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_nanortc_wait_flag(&nano.dc_open, INTEROP_TURN_TIMEOUT_MS);
    ASSERT_OK(rc);
    assert_relay_path(&nano);

    /* libdatachannel → nanortc, via relay */
    const char *to_nano = "hello via TURN relay";
    int initial_nano = atomic_load(&nano.msg_count);
    rc = interop_libdatachannel_send_string(&ldc, to_nano);
    ASSERT_TRUE(rc >= 0);

    uint32_t start = interop_get_millis();
    while (atomic_load(&nano.msg_count) <= initial_nano) {
        ASSERT_TRUE(interop_get_millis() - start < INTEROP_TURN_TIMEOUT_MS);
        interop_sleep_ms(10);
    }

    pthread_mutex_lock(&nano.msg_mutex);
    ASSERT_TRUE(nano.last_msg_is_string);
    ASSERT_EQ(nano.last_msg_len, strlen(to_nano));
    ASSERT_TRUE(memcmp(nano.last_msg, to_nano, strlen(to_nano)) == 0);
    pthread_mutex_unlock(&nano.msg_mutex);

    /* nanortc → libdatachannel, via relay */
    const char *to_ldc = "ack via TURN relay";
    int initial_ldc = atomic_load(&ldc.msg_count);
    rc = nanortc_datachannel_send_string(&nano.rtc, 0, to_ldc);
    ASSERT_OK(rc);

    start = interop_get_millis();
    while (atomic_load(&ldc.msg_count) <= initial_ldc) {
        ASSERT_TRUE(interop_get_millis() - start < INTEROP_TURN_TIMEOUT_MS);
        interop_sleep_ms(10);
    }

    pthread_mutex_lock(&ldc.msg_mutex);
    ASSERT_TRUE(ldc.last_msg_is_string);
    ASSERT_EQ(ldc.last_msg_len, strlen(to_ldc));
    ASSERT_TRUE(memcmp(ldc.last_msg, to_ldc, strlen(to_ldc)) == 0);
    pthread_mutex_unlock(&ldc.msg_mutex);

    assert_relay_path(&nano);
    teardown_relay_pair(&pipe, &nano, &ldc);
}

/* ----------------------------------------------------------------
 * Test 3: ChannelData burst — many small messages through the relay
 * ---------------------------------------------------------------- */

TEST(test_relay_only_channel_data_burst)
{
    SKIP_IF_NO_TURN();

    interop_sig_pipe_t pipe;
    interop_nanortc_peer_t nano;
    interop_libdatachannel_peer_t ldc;

    int rc = setup_relay_pair(&pipe, &nano, &ldc, "relay-burst");
    ASSERT_OK(rc);

    rc = interop_libdatachannel_wait_flag(&ldc.dc_open, INTEROP_TURN_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_nanortc_wait_flag(&nano.dc_open, INTEROP_TURN_TIMEOUT_MS);
    ASSERT_OK(rc);
    assert_relay_path(&nano);

    const int burst = 30; /* enough to exercise queueing without saturating coturn */
    int initial = atomic_load(&nano.msg_count);

    char buf[64];
    for (int i = 0; i < burst; i++) {
        snprintf(buf, sizeof(buf), "burst-msg-%04d", i);
        rc = interop_libdatachannel_send_string(&ldc, buf);
        ASSERT_TRUE(rc >= 0);
        interop_sleep_ms(5);
    }

    /* Wait until all are received. */
    uint32_t start = interop_get_millis();
    while (atomic_load(&nano.msg_count) < initial + burst) {
        ASSERT_TRUE(interop_get_millis() - start < INTEROP_TURN_TIMEOUT_MS);
        interop_sleep_ms(20);
    }

    ASSERT_EQ(atomic_load(&nano.msg_count), initial + burst);
    assert_relay_path(&nano);
    teardown_relay_pair(&pipe, &nano, &ldc);
}

/* ----------------------------------------------------------------
 * Test 4: large payload close to SCTP MTU through relay
 * ---------------------------------------------------------------- */

TEST(test_relay_only_large_payload)
{
    SKIP_IF_NO_TURN();

    interop_sig_pipe_t pipe;
    interop_nanortc_peer_t nano;
    interop_libdatachannel_peer_t ldc;

    int rc = setup_relay_pair(&pipe, &nano, &ldc, "relay-large");
    ASSERT_OK(rc);

    rc = interop_libdatachannel_wait_flag(&ldc.dc_open, INTEROP_TURN_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_nanortc_wait_flag(&nano.dc_open, INTEROP_TURN_TIMEOUT_MS);
    ASSERT_OK(rc);
    assert_relay_path(&nano);

    /* 1100 bytes — close to a typical SCTP DATA chunk MTU when wrapped in
     * DTLS+TURN ChannelData. Larger than this and SCTP fragments. */
    char payload[1100];
    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (char)('a' + (i % 26));
    }

    int initial = atomic_load(&nano.msg_count);
    rc = interop_libdatachannel_send_binary(&ldc, payload, sizeof(payload));
    ASSERT_TRUE(rc >= 0);

    uint32_t start = interop_get_millis();
    while (atomic_load(&nano.msg_count) <= initial) {
        ASSERT_TRUE(interop_get_millis() - start < INTEROP_TURN_TIMEOUT_MS);
        interop_sleep_ms(20);
    }

    pthread_mutex_lock(&nano.msg_mutex);
    ASSERT_FALSE(nano.last_msg_is_string);
    ASSERT_EQ(nano.last_msg_len, sizeof(payload));
    ASSERT_TRUE(memcmp(nano.last_msg, payload, sizeof(payload)) == 0);
    pthread_mutex_unlock(&nano.msg_mutex);

    assert_relay_path(&nano);
    teardown_relay_pair(&pipe, &nano, &ldc);
}

/* ----------------------------------------------------------------
 * Test 5: echo round-trip (libdc → nano → libdc) entirely via relay
 * ---------------------------------------------------------------- */

TEST(test_relay_only_echo_roundtrip)
{
    SKIP_IF_NO_TURN();

    interop_sig_pipe_t pipe;
    interop_nanortc_peer_t nano;
    interop_libdatachannel_peer_t ldc;

    int rc = setup_relay_pair(&pipe, &nano, &ldc, "relay-echo");
    ASSERT_OK(rc);

    rc = interop_libdatachannel_wait_flag(&ldc.dc_open, INTEROP_TURN_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_nanortc_wait_flag(&nano.dc_open, INTEROP_TURN_TIMEOUT_MS);
    ASSERT_OK(rc);
    assert_relay_path(&nano);

    const char *request = "echo-request";
    int initial_nano = atomic_load(&nano.msg_count);
    rc = interop_libdatachannel_send_string(&ldc, request);
    ASSERT_TRUE(rc >= 0);

    uint32_t start = interop_get_millis();
    while (atomic_load(&nano.msg_count) <= initial_nano) {
        ASSERT_TRUE(interop_get_millis() - start < INTEROP_TURN_TIMEOUT_MS);
        interop_sleep_ms(10);
    }

    pthread_mutex_lock(&nano.msg_mutex);
    ASSERT_EQ(nano.last_msg_len, strlen(request));
    ASSERT_TRUE(memcmp(nano.last_msg, request, strlen(request)) == 0);
    pthread_mutex_unlock(&nano.msg_mutex);

    /* nanortc echoes back via relay. */
    const char *reply = "echo-reply";
    int initial_ldc = atomic_load(&ldc.msg_count);
    rc = nanortc_datachannel_send_string(&nano.rtc, 0, reply);
    ASSERT_OK(rc);

    start = interop_get_millis();
    while (atomic_load(&ldc.msg_count) <= initial_ldc) {
        ASSERT_TRUE(interop_get_millis() - start < INTEROP_TURN_TIMEOUT_MS);
        interop_sleep_ms(10);
    }

    pthread_mutex_lock(&ldc.msg_mutex);
    ASSERT_EQ(ldc.last_msg_len, strlen(reply));
    ASSERT_TRUE(memcmp(ldc.last_msg, reply, strlen(reply)) == 0);
    pthread_mutex_unlock(&ldc.msg_mutex);

    assert_relay_path(&nano);
    teardown_relay_pair(&pipe, &nano, &ldc);
}

/* ----------------------------------------------------------------
 * Test runner
 * ---------------------------------------------------------------- */

int main(void)
{
    load_ice_config();
    turn_server_reachable = probe_turn_server();

    printf("TURN relay-only interop test\n");
    printf("  STUN: %s\n", stun_url);
    printf("  TURN: %s user=%s\n", turn_url, turn_user);
    printf("  TURN reachable: %s\n", turn_server_reachable ? "yes" : "no");
    if (!turn_server_reachable) {
        printf("  → all tests will be SKIPPED. Start a TURN server with:\n");
        printf("       ./scripts/start-test-turn.sh\n");
        printf("    or override with NANORTC_TURN_URL/USER/PASS environment.\n");
    }

    UNITY_BEGIN();
    RUN_TEST(test_relay_only_handshake);
    RUN_TEST(test_relay_only_dc_string_bidirectional);
    RUN_TEST(test_relay_only_channel_data_burst);
    RUN_TEST(test_relay_only_large_payload);
    RUN_TEST(test_relay_only_echo_roundtrip);
    return UNITY_END();
}
