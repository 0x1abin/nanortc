/*
 * nanortc interop tests — TURN relay-only data path, nanortc side
 *
 * Mirror image of test_interop_turn_relay.c: this time nanortc is the TURN
 * client. nanortc is configured with TURN only (no STUN, intentionally —
 * configuring STUN too would let nanortc discover its own srflx candidate
 * and bake the public IP into the answer SDP, giving libdc a direct path
 * that bypasses the relay) and runs in relay_only mode (see
 * interop_nanortc_ice_config_t.relay_only), which skips the host candidate
 * in the SDP — only the TURN-allocated relay candidate is advertised.
 * libdatachannel stays host-only, so the only reachable address libdc has
 * to talk to is nanortc's relay on coturn.
 *
 * Every byte nanortc sends must be wrapped through the TURN server:
 *   - outbound ICE connectivity checks → Send Indication / ChannelData
 *   - inbound ICE from libdc → Data Indication → unwrapped into ice_handle_stun
 *     with via_turn=1 (F6) → selected_type flips to RELAY
 *   - DTLS/SCTP data path → lazy wrap in nanortc_poll_output() (F7)
 *   - consent-freshness keepalive → wrapped on RELAY pair (F8)
 *   - CreatePermission for libdc's host candidate (trickled after relay
 *     allocation) → per-tick permission fan-out (F9)
 *   - stats_enqueue_via_turn counter increments on every wrapped packet (F10)
 *
 * Default TURN server: turn:127.0.0.1:3478 (local coturn from
 * scripts/start-test-turn.sh). Override via environment:
 *   NANORTC_TURN_URL  e.g. "turn:turn-cn.uipcat.com:3478"
 *   NANORTC_TURN_USER e.g. "alice"
 *   NANORTC_TURN_PASS e.g. "secret"
 *   NANORTC_STUN_URL  e.g. "stun:stun.l.google.com:19302"
 *
 * If the configured TURN server is unreachable, all tests are skipped so
 * CI without a TURN server stays green.
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
static int turn_host_is_loopback = 0;

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

/* Extract the host portion of a "turn:HOST:PORT" / "turns:HOST:PORT" /
 * "turn:[HOST]:PORT" URI into @p out (NUL-terminated). Handles bracketed
 * IPv6 per RFC 7065 §3.1. Returns 1 on success, 0 on malformed input. */
static int parse_turn_host(const char *uri, char *out, size_t out_size)
{
    const char *p = uri;
    if (strncmp(p, "turn:", 5) == 0)
        p += 5;
    else if (strncmp(p, "turns:", 6) == 0)
        p += 6;
    else
        return 0;

    const char *host_start = p;
    const char *host_end;
    if (*p == '[') {
        /* Bracketed IPv6: turn:[::1]:3478 */
        host_start = p + 1;
        host_end = strchr(host_start, ']');
        if (!host_end)
            return 0;
    } else {
        /* Bare host[:port] — port is optional in the URI grammar */
        host_end = strrchr(p, ':');
        if (!host_end)
            host_end = p + strlen(p); /* no port */
    }
    size_t host_len = (size_t)(host_end - host_start);
    if (host_len == 0 || host_len >= out_size)
        return 0;
    memcpy(out, host_start, host_len);
    out[host_len] = '\0';
    return 1;
}

/* Return 1 if the TURN host resolves to a loopback address. Used to auto-skip
 * the strict relay-path assertions when the test can't actually force data
 * through the relay: on a same-host loopback setup, libdc's source IP as seen
 * by coturn is 127.0.0.1 while nanortc installs permissions only for libdc's
 * advertised host candidates (LAN IPs), so coturn silently drops libdc's
 * checks. The existing turnserver.conf additionally has `no-loopback-peers`
 * which rejects loopback peer targets outright. The test body itself is sound
 * and runs strictly against any non-loopback TURN server — override via
 * NANORTC_TURN_URL, USER, PASS to point at a real relay and the assertions
 * will take effect.
 *
 * Uses getaddrinfo(AF_UNSPEC) so bracketed IPv6 URIs and DNS names both work. */
static int is_loopback_turn(void)
{
    char host[256];
    if (!parse_turn_host(turn_url, host, sizeof(host)))
        return 0;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res)
        return 0;

    int loopback = 0;
    for (struct addrinfo *ai = res; ai && !loopback; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET) {
            const struct sockaddr_in *sa = (const struct sockaddr_in *)ai->ai_addr;
            /* 127.0.0.0/8 */
            if ((ntohl(sa->sin_addr.s_addr) & 0xff000000u) == 0x7f000000u)
                loopback = 1;
        } else if (ai->ai_family == AF_INET6) {
            const struct sockaddr_in6 *sa = (const struct sockaddr_in6 *)ai->ai_addr;
            static const uint8_t loopback6[16] = {0, 0, 0, 0, 0, 0, 0, 0,
                                                  0, 0, 0, 0, 0, 0, 0, 1};
            if (memcmp(&sa->sin6_addr, loopback6, 16) == 0)
                loopback = 1;
        }
    }
    freeaddrinfo(res);
    return loopback;
}

/* Probe whether the configured TURN server is live by sending a real STUN
 * Binding Request. Coturn answers Binding requests without authentication,
 * so a successful Binding round-trip proves the server is up. We can't
 * just rely on sendto() to 127.0.0.1: loopback delivers packets to a
 * non-existent listener without error.
 * Returns 1 if reachable, 0 otherwise. Same logic as test_interop_turn_relay.c. */
static int probe_turn_server(void)
{
    char host[256];
    if (!parse_turn_host(turn_url, host, sizeof(host))) {
        return 0;
    }

    /* Extract port: skip the scheme prefix, then locate the last ':' AFTER
     * the optional bracketed-IPv6 closer. parse_turn_host has already
     * validated the host portion. */
    const char *p = turn_url;
    p += (strncmp(p, "turns:", 6) == 0) ? 6 : 5;
    const char *port_anchor = (*p == '[') ? strchr(p, ']') : p;
    if (!port_anchor) {
        return 0;
    }
    const char *colon = strrchr(port_anchor, ':');
    if (!colon || colon < port_anchor) {
        return 0;
    }
    int port = atoi(colon + 1);
    if (port <= 0 || port > 65535) {
        return 0;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
        return 0;
    }

    int fd = -1;
    struct sockaddr_storage sa;
    socklen_t sa_len = 0;
    memset(&sa, 0, sizeof(sa));
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        if (ai->ai_addr == NULL || ai->ai_addrlen > sizeof(sa))
            continue;
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        memcpy(&sa, ai->ai_addr, ai->ai_addrlen);
        sa_len = (socklen_t)ai->ai_addrlen;
        if (ai->ai_family == AF_INET) {
            ((struct sockaddr_in *)&sa)->sin_port = htons((uint16_t)port);
        } else if (ai->ai_family == AF_INET6) {
            ((struct sockaddr_in6 *)&sa)->sin6_port = htons((uint16_t)port);
        }
        break;
    }
    freeaddrinfo(res);
    if (fd < 0 || sa_len == 0) {
        return 0;
    }

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
        if (sendto(fd, req, sizeof(req), 0, (struct sockaddr *)&sa, sa_len) < 0) {
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
            if (n >= 20 && resp[0] == 0x01 && resp[1] == 0x01 && memcmp(resp + 4, req + 4, 16) == 0) {
                reachable = 1;
            }
        }
    }

    close(fd);
    return reachable;
}

/* ----------------------------------------------------------------
 * Port allocation — separate range from other TURN tests
 * ---------------------------------------------------------------- */

static uint16_t next_relay_port = INTEROP_TURN_PORT_BASE + 100;

static uint16_t alloc_relay_port(void)
{
    return next_relay_port++;
}

/* ----------------------------------------------------------------
 * Per-test ICE config storage (avoids stale pointers between tests)
 * ---------------------------------------------------------------- */

#define MAX_RELAY_NANORTC_TESTS 6

static const char *turn_url_ptrs[MAX_RELAY_NANORTC_TESTS];
static nanortc_ice_server_t ice_servers_store[MAX_RELAY_NANORTC_TESTS][1];
static char resolve_scratches[MAX_RELAY_NANORTC_TESTS][1024];
static int setup_idx;

/* ----------------------------------------------------------------
 * relay-only nanortc pair setup
 *
 * nanortc: STUN+TURN configured AND relay_only=1 (skips host candidate).
 *          Its SDP advertises only the TURN relay candidate.
 * libdc:   host-only (no ICE servers at all). Uses its loopback/LAN
 *          host candidates.
 *
 * libdc's host candidates can only reach nanortc via the TURN server,
 * because nanortc has no direct-reachable address. coturn unwraps libdc's
 * UDP into Data Indications on nanortc's client socket, and nanortc wraps
 * every outbound packet as Send Indication / ChannelData.
 * ---------------------------------------------------------------- */

static int setup_relay_pair_nanortc(interop_sig_pipe_t *pipe, interop_nanortc_peer_t *nano,
                                    interop_libdatachannel_peer_t *ldc, const char *dc_label)
{
    uint16_t port = alloc_relay_port();
    int idx = setup_idx++;

    if (idx >= MAX_RELAY_NANORTC_TESTS) {
        fprintf(stderr, "[test] MAX_RELAY_NANORTC_TESTS exhausted\n");
        return -1;
    }

    if (interop_sig_create(pipe) != 0) {
        fprintf(stderr, "[test] Failed to create signaling pipe\n");
        return -1;
    }

    /* relay-only: pass ONLY the TURN server, no STUN. If we also passed the
     * STUN server, nanortc would discover its own public IP via srflx and
     * bake it into the answer SDP, giving libdc a direct-reachable address
     * that bypasses the TURN relay entirely. */
    turn_url_ptrs[idx] = turn_url;

    ice_servers_store[idx][0] = (nanortc_ice_server_t){.urls = &turn_url_ptrs[idx],
                                                       .url_count = 1,
                                                       .username = turn_user,
                                                       .credential = turn_pass};

    interop_nanortc_ice_config_t nano_ice = {
        .ice_servers = ice_servers_store[idx],
        .ice_server_count = 1,
        .resolve_scratch = resolve_scratches[idx],
        .resolve_scratch_size = sizeof(resolve_scratches[idx]),
        .relay_only = 1,
    };

    fprintf(stderr, "[test] TURN (nanortc): %s user=%s (no STUN in relay-only mode)\n", turn_url,
            turn_user);

    /* nanortc: STUN+TURN + relay_only (no host candidate in SDP). */
    if (interop_nanortc_start(nano, pipe->fd[0], port, &nano_ice) != 0) {
        fprintf(stderr, "[test] Failed to start nanortc peer\n");
        interop_sig_destroy(pipe);
        return -1;
    }

    /* libdatachannel: host-only (no ICE servers). Pass remote_port=0 to
     * suppress the direct-host fallback injection at
     * interop_libdatachannel_peer.c:141 — otherwise libdc gets a
     * 127.0.0.1:<nanortc_port> typ host candidate handed to it and reaches
     * nanortc directly, bypassing the TURN relay entirely. */
    if (interop_libdatachannel_start(ldc, pipe->fd[1], dc_label, 0) != 0) {
        fprintf(stderr, "[test] Failed to start libdatachannel peer\n");
        interop_nanortc_stop(nano);
        interop_sig_destroy(pipe);
        return -1;
    }

    return 0;
}

static void teardown_relay_pair_nanortc(interop_sig_pipe_t *pipe, interop_nanortc_peer_t *nano,
                                        interop_libdatachannel_peer_t *ldc)
{
    interop_libdatachannel_stop(ldc);
    interop_nanortc_stop(nano);
    interop_sig_destroy(pipe);
}

/* Assert that nanortc actually routed its own bytes through the TURN server.
 *
 * This is the whole reason this test exists. It verifies the five Phase 5.2
 * client-side relay fixes were effective end-to-end:
 *
 *   (1) ice.selected_type == RELAY — proves the via_turn signal (F6) reached
 *       ICE on the USE-CANDIDATE path. Without F6, selected_type would stay
 *       HOST even when STUN checks arrive unwrapped from a Data Indication.
 *
 *   (2) stats_enqueue_via_turn > 0 — proves the lazy wrap path in
 *       nanortc_poll_output() (F7) actually fired. Without F7, bytes would
 *       either leak direct or (in the earlier eager-wrap bug) corrupt each
 *       other in shared scratch.
 *
 *   (3) stats_wrap_dropped == 0 — proves no packet was silently lost during
 *       the lazy wrap stage (e.g. turn_buf overflow).
 *
 *   (4) stats_tx_queue_full == 0 — proves out_queue / out_wrap_meta side
 *       table did not overflow under bursty load.
 *
 * We intentionally do NOT assert stats_enqueue_direct == 0. TURN control
 * plane traffic (Allocate, Refresh, CreatePermission, ChannelBind) is sent
 * direct-to-coturn, not wrapped through it, so stats_enqueue_direct > 0 is
 * both expected and healthy. */
/* Relaxed-atomic read helpers.
 *
 * The peer thread concurrently updates ice.selected_type and the stats_*
 * counters while the test thread reads them here. Both sides use
 * __atomic_* with relaxed ordering — writers in src/nano_rtc.c and
 * src/nano_ice.c (__atomic_fetch_add for the monotone counters,
 * __atomic_store_n for selected_type), readers here. That makes the
 * access well-defined under the C memory model and TSan-clean while
 * compiling to the same machine code as plain ++ / = on every supported
 * target. Monotonicity of the counters + the single nomination write of
 * selected_type means relaxed ordering is sufficient for our
 * eventually-consistent assertions. */
static inline uint8_t read_selected_type(const interop_nanortc_peer_t *nano)
{
    return __atomic_load_n(&nano->rtc.ice.selected_type, __ATOMIC_RELAXED);
}
static inline uint32_t read_stat_u32(const uint32_t *p)
{
    return __atomic_load_n(p, __ATOMIC_RELAXED);
}

static void assert_nanortc_relay_path(const interop_nanortc_peer_t *nano)
{
    uint8_t selected_type = read_selected_type(nano);
    uint32_t via_turn = read_stat_u32(&nano->rtc.stats_enqueue_via_turn);
    uint32_t wrap_dropped = read_stat_u32(&nano->rtc.stats_wrap_dropped);
    uint32_t tx_queue_full = read_stat_u32(&nano->rtc.stats_tx_queue_full);

    TEST_ASSERT_EQUAL_MESSAGE(NANORTC_ICE_CAND_RELAY, selected_type,
                              "nanortc ice.selected_type != RELAY "
                              "(Phase 5.2 F6 via_turn signal not effective?)");

    TEST_ASSERT_TRUE_MESSAGE(via_turn > 0, "stats_enqueue_via_turn == 0 "
                                           "(Phase 5.2 F7 lazy wrap never fired?)");

    TEST_ASSERT_EQUAL_MESSAGE(0, wrap_dropped, "stats_wrap_dropped > 0 "
                                               "(lazy wrap lost packets, check turn_buf sizing)");

    TEST_ASSERT_EQUAL_MESSAGE(0, tx_queue_full, "stats_tx_queue_full > 0 "
                                                "(out_queue overflowed under test load)");
}

/* ----------------------------------------------------------------
 * Skip-on-no-server guard
 * ---------------------------------------------------------------- */

#define SKIP_IF_NO_TURN()                                                                          \
    do {                                                                                           \
        if (!turn_server_reachable) {                                                              \
            fprintf(stderr, "[test] SKIP: TURN server unreachable (%s)\n", turn_url);              \
            return;                                                                                \
        }                                                                                          \
        if (turn_host_is_loopback) {                                                               \
            fprintf(stderr,                                                                        \
                    "[test] SKIP: loopback TURN cannot force nanortc relay path — "                \
                    "libdc's loopback source bypasses coturn permissions. Re-run with "            \
                    "NANORTC_TURN_URL pointing at a non-loopback TURN server to exercise "         \
                    "the Phase 5.2 wrap path end-to-end.\n");                                      \
            return;                                                                                \
        }                                                                                          \
    } while (0)

/* ----------------------------------------------------------------
 * Test 1: handshake — ICE+DTLS+SCTP with nanortc relay-only
 * ---------------------------------------------------------------- */

TEST(test_relay_nanortc_handshake)
{
    SKIP_IF_NO_TURN();

    interop_sig_pipe_t pipe;
    interop_nanortc_peer_t nano;
    interop_libdatachannel_peer_t ldc;

    int rc = setup_relay_pair_nanortc(&pipe, &nano, &ldc, "relay-nanortc-hs");
    ASSERT_OK(rc);

    rc = interop_libdatachannel_wait_flag(&ldc.connected, INTEROP_TURN_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_nanortc_wait_flag(&nano.sctp_connected, INTEROP_TURN_TIMEOUT_MS);
    ASSERT_OK(rc);

    ASSERT_TRUE(atomic_load(&nano.ice_connected));
    ASSERT_TRUE(atomic_load(&nano.dtls_connected));
    ASSERT_TRUE(atomic_load(&nano.sctp_connected));
    assert_nanortc_relay_path(&nano);

    teardown_relay_pair_nanortc(&pipe, &nano, &ldc);
}

/* ----------------------------------------------------------------
 * Test 2: bidirectional DC strings — nanortc wraps every byte via TURN
 * ---------------------------------------------------------------- */

TEST(test_relay_nanortc_dc_string_bidirectional)
{
    SKIP_IF_NO_TURN();

    interop_sig_pipe_t pipe;
    interop_nanortc_peer_t nano;
    interop_libdatachannel_peer_t ldc;

    int rc = setup_relay_pair_nanortc(&pipe, &nano, &ldc, "relay-nanortc-string");
    ASSERT_OK(rc);

    rc = interop_libdatachannel_wait_flag(&ldc.dc_open, INTEROP_TURN_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_nanortc_wait_flag(&nano.dc_open, INTEROP_TURN_TIMEOUT_MS);
    ASSERT_OK(rc);
    assert_nanortc_relay_path(&nano);

    /* libdatachannel → nanortc, via relay */
    const char *to_nano = "hello nanortc relay-client";
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

    /* nanortc → libdatachannel: this is the direction we care about most.
     * Capture the via_turn counter before the send and verify it grew. */
    uint32_t via_before = read_stat_u32(&nano.rtc.stats_enqueue_via_turn);

    const char *to_ldc = "ack from nanortc relay-client";
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

    TEST_ASSERT_TRUE_MESSAGE(read_stat_u32(&nano.rtc.stats_enqueue_via_turn) > via_before,
                             "stats_enqueue_via_turn did not grow after nanortc send "
                             "(bytes leaked direct instead of wrapping through TURN)");

    assert_nanortc_relay_path(&nano);
    teardown_relay_pair_nanortc(&pipe, &nano, &ldc);
}

/* ----------------------------------------------------------------
 * Test 3: ChannelData burst — many small messages wrapped by nanortc
 * ---------------------------------------------------------------- */

TEST(test_relay_nanortc_channel_data_burst)
{
    SKIP_IF_NO_TURN();

    interop_sig_pipe_t pipe;
    interop_nanortc_peer_t nano;
    interop_libdatachannel_peer_t ldc;

    int rc = setup_relay_pair_nanortc(&pipe, &nano, &ldc, "relay-nanortc-burst");
    ASSERT_OK(rc);

    rc = interop_libdatachannel_wait_flag(&ldc.dc_open, INTEROP_TURN_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_nanortc_wait_flag(&nano.dc_open, INTEROP_TURN_TIMEOUT_MS);
    ASSERT_OK(rc);
    assert_nanortc_relay_path(&nano);

    /* Send the burst from nanortc's side so the lazy-wrap path gets
     * hammered with back-to-back enqueues, exercising F7's side-table. */
    const int burst = 30;
    int initial = atomic_load(&ldc.msg_count);
    uint32_t via_before = read_stat_u32(&nano.rtc.stats_enqueue_via_turn);

    char buf[64];
    for (int i = 0; i < burst; i++) {
        snprintf(buf, sizeof(buf), "burst-msg-%04d", i);
        rc = nanortc_datachannel_send_string(&nano.rtc, 0, buf);
        ASSERT_OK(rc);
        interop_sleep_ms(5);
    }

    uint32_t start = interop_get_millis();
    while (atomic_load(&ldc.msg_count) < initial + burst) {
        ASSERT_TRUE(interop_get_millis() - start < INTEROP_TURN_TIMEOUT_MS);
        interop_sleep_ms(20);
    }

    ASSERT_EQ(atomic_load(&ldc.msg_count), initial + burst);

    TEST_ASSERT_TRUE_MESSAGE(read_stat_u32(&nano.rtc.stats_enqueue_via_turn) >=
                                 via_before + (uint32_t)burst,
                             "stats_enqueue_via_turn did not grow by at least burst count "
                             "(some outbound bytes did not take the wrap path)");

    assert_nanortc_relay_path(&nano);
    teardown_relay_pair_nanortc(&pipe, &nano, &ldc);
}

/* ----------------------------------------------------------------
 * Test 4: large payload close to SCTP MTU — nanortc wraps in one pkt
 * ---------------------------------------------------------------- */

TEST(test_relay_nanortc_large_payload)
{
    SKIP_IF_NO_TURN();

    interop_sig_pipe_t pipe;
    interop_nanortc_peer_t nano;
    interop_libdatachannel_peer_t ldc;

    int rc = setup_relay_pair_nanortc(&pipe, &nano, &ldc, "relay-nanortc-large");
    ASSERT_OK(rc);

    rc = interop_libdatachannel_wait_flag(&ldc.dc_open, INTEROP_TURN_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_nanortc_wait_flag(&nano.dc_open, INTEROP_TURN_TIMEOUT_MS);
    ASSERT_OK(rc);
    assert_nanortc_relay_path(&nano);

    /* 1100 bytes — close to a typical SCTP DATA chunk MTU when wrapped in
     * DTLS+TURN ChannelData. Sent from nanortc so the big payload traverses
     * the turn_buf lazy wrap path. */
    char payload[1100];
    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (char)('a' + (i % 26));
    }

    uint32_t via_before = read_stat_u32(&nano.rtc.stats_enqueue_via_turn);
    int initial = atomic_load(&ldc.msg_count);
    rc = nanortc_datachannel_send(&nano.rtc, 0, payload, sizeof(payload));
    ASSERT_OK(rc);

    uint32_t start = interop_get_millis();
    while (atomic_load(&ldc.msg_count) <= initial) {
        ASSERT_TRUE(interop_get_millis() - start < INTEROP_TURN_TIMEOUT_MS);
        interop_sleep_ms(20);
    }

    pthread_mutex_lock(&ldc.msg_mutex);
    ASSERT_FALSE(ldc.last_msg_is_string);
    ASSERT_EQ(ldc.last_msg_len, sizeof(payload));
    ASSERT_TRUE(memcmp(ldc.last_msg, payload, sizeof(payload)) == 0);
    pthread_mutex_unlock(&ldc.msg_mutex);

    TEST_ASSERT_TRUE_MESSAGE(read_stat_u32(&nano.rtc.stats_enqueue_via_turn) > via_before,
                             "stats_enqueue_via_turn did not grow after large send");

    assert_nanortc_relay_path(&nano);
    teardown_relay_pair_nanortc(&pipe, &nano, &ldc);
}

/* ----------------------------------------------------------------
 * Test 5: echo round-trip — both directions via relay
 * ---------------------------------------------------------------- */

TEST(test_relay_nanortc_echo_roundtrip)
{
    SKIP_IF_NO_TURN();

    interop_sig_pipe_t pipe;
    interop_nanortc_peer_t nano;
    interop_libdatachannel_peer_t ldc;

    int rc = setup_relay_pair_nanortc(&pipe, &nano, &ldc, "relay-nanortc-echo");
    ASSERT_OK(rc);

    rc = interop_libdatachannel_wait_flag(&ldc.dc_open, INTEROP_TURN_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_nanortc_wait_flag(&nano.dc_open, INTEROP_TURN_TIMEOUT_MS);
    ASSERT_OK(rc);
    assert_nanortc_relay_path(&nano);

    const char *request = "echo-request-relay-nanortc";
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

    /* nanortc echoes back — this hop is wrapped through TURN. */
    uint32_t via_before = read_stat_u32(&nano.rtc.stats_enqueue_via_turn);
    const char *reply = "echo-reply-relay-nanortc";
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

    TEST_ASSERT_TRUE_MESSAGE(read_stat_u32(&nano.rtc.stats_enqueue_via_turn) > via_before,
                             "echo reply did not take the wrap path");

    assert_nanortc_relay_path(&nano);
    teardown_relay_pair_nanortc(&pipe, &nano, &ldc);
}

/* ----------------------------------------------------------------
 * Test runner
 * ---------------------------------------------------------------- */

int main(void)
{
    load_ice_config();
    turn_server_reachable = probe_turn_server();
    turn_host_is_loopback = is_loopback_turn();

    printf("TURN relay-only nanortc-client interop test\n");
    printf("  STUN: %s\n", stun_url);
    printf("  TURN: %s user=%s\n", turn_url, turn_user);
    printf("  TURN reachable: %s\n", turn_server_reachable ? "yes" : "no");
    printf("  TURN loopback: %s\n", turn_host_is_loopback ? "yes (tests will SKIP)" : "no");
    if (!turn_server_reachable) {
        printf("  → all tests will be SKIPPED. Start a TURN server with:\n");
        printf("       ./scripts/start-test-turn.sh\n");
        printf("    or override with NANORTC_TURN_URL/USER/PASS environment.\n");
    }

    UNITY_BEGIN();
    RUN_TEST(test_relay_nanortc_handshake);
    RUN_TEST(test_relay_nanortc_dc_string_bidirectional);
    RUN_TEST(test_relay_nanortc_channel_data_burst);
    RUN_TEST(test_relay_nanortc_large_payload);
    RUN_TEST(test_relay_nanortc_echo_roundtrip);
    return UNITY_END();
}
