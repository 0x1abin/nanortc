/* Issue #81: two private endpoints behind independent UDP NATs.
 * RFC 4787 §5: endpoint-independent mapping, address/port-dependent filtering.
 * Contacting STUN establishes a mapping but cannot open the peer's filter.
 * No sockets, external STUN/TURN, sleeps or privileged network namespaces. */
#include "nanortc.h"
#include "nano_stun.h"
#include "nano_test.h"
#include "nano_test_config.h"
#include <string.h>

#if NANORTC_FEATURE_DATACHANNEL && NANORTC_FEATURE_ICE_SRFLX
#define NAT_ENDPOINTS 2
static nanortc_t peers[NAT_ENDPOINTS];
static const nanortc_addr_t private_addr[NAT_ENDPOINTS] = {
    {.addr = {10, 0, 0, 1}, .port = 4000, .family = 4},
    {.addr = {10, 1, 0, 1}, .port = 5000, .family = 4},
};
static const nanortc_addr_t public_addr[NAT_ENDPOINTS] = {
    {.addr = {198, 51, 100, 1}, .port = 14000, .family = 4},
    {.addr = {203, 0, 113, 1}, .port = 15000, .family = 4},
};
static bool opened[NAT_ENDPOINTS], mapped[NAT_ENDPOINTS];
static unsigned dropped, received[NAT_ENDPOINTS];
#define NAT_QUEUE_SIZE  32
#define NAT_PACKET_SIZE 2048
typedef struct {
    uint8_t data[NAT_PACKET_SIZE];
    size_t len;
} nat_packet_t;
static nat_packet_t packets[NAT_ENDPOINTS][NAT_QUEUE_SIZE];
static unsigned head[NAT_ENDPOINTS], tail[NAT_ENDPOINTS];
static void nat_deliver(unsigned side, uint32_t now)
{
    while (head[side] != tail[side]) {
        nat_packet_t *p = &packets[side][head[side] % NAT_QUEUE_SIZE];
        int rc = nanortc_handle_input(&peers[side], &(nanortc_input_t){.now_ms = now,
                                                                       .data = p->data,
                                                                       .len = p->len,
                                                                       .src = public_addr[1 - side],
                                                                       .dst = private_addr[side]});
        if (rc == NANORTC_ERR_WOULD_BLOCK)
            return; /* Drain output before retrying input. */
        ASSERT_TRUE(rc == NANORTC_OK || rc == NANORTC_ERR_PROTOCOL || rc == NANORTC_ERR_STATE);
        head[side]++;
    }
}
static bool same_addr(const nanortc_addr_t *a, const nanortc_addr_t *b)
{
    return a->family == b->family && a->port == b->port &&
           memcmp(a->addr, b->addr, sizeof(a->addr)) == 0;
}

static void nat_drain(unsigned side, uint32_t now, bool signal_candidates)
{
    nanortc_t *from = &peers[side], *to = &peers[1 - side];
    nanortc_output_t out;
    while (nanortc_poll_output(from, &out) == NANORTC_OK) {
        if (out.type == NANORTC_OUTPUT_EVENT) {
            if (out.event.type == NANORTC_EV_ICE_CANDIDATE && signal_candidates &&
                strstr(out.event.ice_candidate.candidate_str, "typ srflx"))
                ASSERT_OK(nanortc_add_remote_candidate(to, out.event.ice_candidate.candidate_str));
            if (out.event.type == NANORTC_EV_DATACHANNEL_DATA) {
                static const uint8_t message[] = {'N', 0, 0xe4, 0xb8, 0xad};
                ASSERT_EQ(out.event.datachannel_data.len, sizeof(message));
                ASSERT_MEM_EQ(out.event.datachannel_data.data, message, sizeof(message));
                received[1 - side]++;
            }
            continue;
        }
        if (out.type != NANORTC_OUTPUT_TRANSMIT)
            continue;
        /* Public mappings are never valid bind/source hints on the host. */
        ASSERT_TRUE(!out.transmit.src.family || same_addr(&out.transmit.src, &private_addr[side]));
        if (out.transmit.dest.port == 3478) {
            stun_msg_t request;
            ASSERT_OK(stun_parse(out.transmit.data, out.transmit.len, &request));
            uint8_t response[256];
            size_t len = 0;
            ASSERT_OK(stun_encode_binding_response(&request, public_addr[side].addr, 1,
                                                   public_addr[side].port, (const uint8_t *)"stun",
                                                   4, nano_test_crypto()->hmac_sha1, response,
                                                   sizeof(response), &len));
            mapped[side] = true;
            ASSERT_OK(nanortc_handle_input(from, &(nanortc_input_t){.now_ms = now,
                                                                    .data = response,
                                                                    .len = len,
                                                                    .src = out.transmit.dest,
                                                                    .dst = private_addr[side]}));
            continue;
        }
        if (!same_addr(&out.transmit.dest, &public_addr[1 - side])) {
            dropped++; /* Private host candidates are not routable across NATs. */
            continue;
        }
        opened[side] = true;
        if (!mapped[1 - side] || !opened[1 - side]) {
            dropped++;
            continue;
        }
        unsigned dest = 1 - side;
        ASSERT_TRUE(tail[dest] - head[dest] < NAT_QUEUE_SIZE);
        nat_packet_t *packet = &packets[dest][tail[dest]++ % NAT_QUEUE_SIZE];
        ASSERT_TRUE(out.transmit.len <= sizeof(packet->data));
        memcpy(packet->data, out.transmit.data, out.transmit.len);
        packet->len = out.transmit.len;
    }
}

static void nat_connect(unsigned offerer, bool host_candidates)
{
    memset(opened, 0, sizeof(opened));
    memset(mapped, 0, sizeof(mapped));
    memset(received, 0, sizeof(received));
    dropped = 0;
    memset(head, 0, sizeof(head));
    memset(tail, 0, sizeof(tail));
    for (unsigned i = 0; i < NAT_ENDPOINTS; i++) {
        nanortc_config_t cfg = {0};
        cfg.crypto = nano_test_crypto();
        cfg.role = i == offerer ? NANORTC_ROLE_CONTROLLING : NANORTC_ROLE_CONTROLLED;
        ASSERT_OK(nanortc_init(&peers[i], &cfg));
        if (host_candidates)
            ASSERT_OK(nanortc_add_local_candidate(&peers[i], i ? "10.1.0.1" : "10.0.0.1",
                                                  private_addr[i].port));
        const char *urls[] = {"stun:192.0.2.100:3478"};
        nanortc_ice_server_t server = {.urls = urls, .url_count = 1};
        ASSERT_OK(nanortc_set_ice_servers(&peers[i], &server, 1));
    }
    int dc = nanortc_create_datachannel(&peers[offerer], "nat", NULL);
    ASSERT_TRUE(dc >= 0);
    char offer[4096], answer[4096];
    size_t len = 0;
    ASSERT_OK(nanortc_create_offer(&peers[offerer], offer, sizeof(offer), &len));
    ASSERT_OK(nanortc_accept_offer(&peers[1 - offerer], offer, answer, sizeof(answer), &len));
    ASSERT_OK(nanortc_accept_answer(&peers[offerer], answer));
    for (uint32_t now = 100; now < 15000; now += 10) {
        for (unsigned i = 0; i < NAT_ENDPOINTS; i++)
            ASSERT_OK(nanortc_handle_input(&peers[i], &(nanortc_input_t){.now_ms = now}));
        for (unsigned n = 0; n < 4; n++) {
            nat_drain(0, now, true);
            nat_drain(1, now, true);
            nat_deliver(0, now);
            nat_deliver(1, now);
        }
        if (peers[offerer].datachannel.channels[0].state == NANORTC_DC_STATE_OPEN)
            break;
    }
    ASSERT_TRUE(dropped > 0);
    ASSERT_TRUE(mapped[0] && mapped[1] && opened[0] && opened[1]);
    if (peers[0].state != NANORTC_STATE_CONNECTED || peers[1].state != NANORTC_STATE_CONNECTED)
        fprintf(stderr, "NAT states: %d/%d ICE: %d/%d checks: %u/%u open: %d/%d dropped: %u\n",
                peers[0].state, peers[1].state, peers[0].ice.state, peers[1].ice.state,
                peers[0].ice.check_count, peers[1].ice.check_count,
                peers[0].datachannel.channels[0].state, peers[1].datachannel.channels[0].state,
                dropped);
    ASSERT_EQ(peers[0].state, NANORTC_STATE_CONNECTED);
    ASSERT_EQ(peers[1].state, NANORTC_STATE_CONNECTED);
    ASSERT_EQ(peers[0].ice.selected_port, public_addr[1].port);
    ASSERT_EQ(peers[1].ice.selected_port, public_addr[0].port);
    static const uint8_t message[] = {'N', 0, 0xe4, 0xb8, 0xad};
    for (unsigned i = 0; i < NAT_ENDPOINTS; i++)
        ASSERT_OK(nanortc_datachannel_send_text(&peers[i], (uint16_t)dc, (const char *)message,
                                                sizeof(message)));
    for (uint32_t now = 15000; now < 16000; now += 10) {
        for (unsigned i = 0; i < NAT_ENDPOINTS; i++) {
            ASSERT_OK(nanortc_handle_input(&peers[i], &(nanortc_input_t){.now_ms = now}));
            nat_drain(i, now, false);
        }
        nat_deliver(0, now);
        nat_deliver(1, now);
        if (received[0] && received[1])
            break;
    }
    ASSERT_EQ(received[0], 1);
    ASSERT_EQ(received[1], 1);
    for (unsigned i = 0; i < NAT_ENDPOINTS; i++)
        nanortc_destroy(&peers[i]);
}
TEST(test_nat_endpoint_zero_offers)
{
    nat_connect(0, true);
}
TEST(test_nat_endpoint_one_offers)
{
    nat_connect(1, true);
}
TEST(test_nat_default_socket)
{
    /* STUN uses the application's default socket before any host candidate
     * is registered. Its public mapping must never become a bind hint. */
    nat_connect(0, false);
    nat_connect(1, false);
}
#endif
TEST_MAIN_BEGIN("ICE NAT")
#if NANORTC_FEATURE_DATACHANNEL && NANORTC_FEATURE_ICE_SRFLX
RUN(test_nat_endpoint_zero_offers);
RUN(test_nat_endpoint_one_offers);
RUN(test_nat_default_socket);
#endif
TEST_MAIN_END
