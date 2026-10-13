/* Deterministic 50 ms RTT, finite-queue link; independent TWCC wire feedback. */
#include "nanortc.h"
#include "nano_test.h"
#include <string.h>
#if NANORTC_FEATURE_VIDEO
#include "nano_bwe.h"
#define LINK_QUEUE_SIZE     512
#define LINK_PACKET_BYTES   1000
#define LINK_FEEDBACK_BYTES 512
#define LINK_STEP_MS        5
#define LINK_RATE_STEPS     (1000 / LINK_STEP_MS)
#define LINK_DOWN_MS        3000
#define LINK_UP_MS          12000
#define LINK_END_MS         30000

typedef struct {
    uint32_t sequence;
    uint32_t sent_ms;
    uint64_t arrival_us;
    bool lost;
} link_packet_t;
static nano_bwe_t bwe;
static link_packet_t queue[LINK_QUEUE_SIZE];
static unsigned head, tail;
static uint8_t feedback_counter;

static void feedback(uint32_t now_ms, uint32_t elapsed_ms)
{
    unsigned count = 0;
    while (head + count < tail && count < NANORTC_TWCC_MAX_PACKETS_PER_FB) {
        const link_packet_t *packet = &queue[(head + count) % LINK_QUEUE_SIZE];
        if ((uint32_t)(now_ms - packet->sent_ms) < 50u ||
            (!packet->lost && packet->arrival_us + 25000u > (uint64_t)elapsed_ms * 1000u))
            break;
        count++;
    }
    while (count && queue[(head + count - 1u) % LINK_QUEUE_SIZE].lost)
        count--; /* The receiver cannot report a gap beyond its latest arrival. */
    if (!count)
        return;
    uint8_t wire[LINK_FEEDBACK_BYTES] = {0x8f, 205};
    nanortc_write_u16be(wire + 12, (uint16_t)queue[head % LINK_QUEUE_SIZE].sequence);
    nanortc_write_u16be(wire + 14, (uint16_t)count);
    wire[19] = feedback_counter++;
    uint64_t first_arrival = 0;
    for (unsigned i = 0; i < count; i++)
        if (!queue[(head + i) % LINK_QUEUE_SIZE].lost) {
            first_arrival = queue[(head + i) % LINK_QUEUE_SIZE].arrival_us;
            break;
        }
    uint64_t reference = first_arrival / 64000u;
    wire[16] = (uint8_t)(reference >> 16);
    wire[17] = (uint8_t)(reference >> 8);
    wire[18] = (uint8_t)reference;
    size_t off = 20;
    /* Two-bit status vectors, seven packets per chunk; large receive deltas.
     * This is the existing RTPFB/FMT15 extension, not RFC 8888's FMT11. */
    for (unsigned i = 0; i < count; i += 7) {
        uint16_t chunk = 0xc000u;
        for (unsigned j = 0; j < 7 && i + j < count; j++)
            if (!queue[(head + i + j) % LINK_QUEUE_SIZE].lost)
                chunk |= (uint16_t)(2u << (12u - 2u * j));
        nanortc_write_u16be(wire + off, chunk);
        off += 2;
    }
    uint64_t previous = reference * 64000u;
    for (unsigned i = 0; i < count; i++) {
        const link_packet_t *packet = &queue[(head + i) % LINK_QUEUE_SIZE];
        if (packet->lost)
            continue;
        uint64_t delta = (packet->arrival_us - previous) / 250u;
        ASSERT_TRUE(delta <= INT16_MAX);
        nanortc_write_u16be(wire + off, (uint16_t)delta);
        off += 2;
        previous = packet->arrival_us;
    }
    off = (off + 3u) & ~(size_t)3u;
    ASSERT_TRUE(off <= sizeof(wire));
    nanortc_write_u16be(wire + 2, (uint16_t)(off / 4u - 1u));
    ASSERT_OK(bwe_on_twcc_feedback(&bwe, wire, off, now_ms));
    head += count;
}

static void simulate(unsigned feedback_ms, bool fixed_cap)
{
    ASSERT_OK(bwe_init(&bwe));
    ASSERT_OK(bwe_set_initial_bitrate(&bwe, 2000000));
    if (fixed_cap) {
        /* Independent REMB vector: 500000 bps = mantissa250000 * 2. */
        const uint8_t cap[] = {0x8f, 206, 0,   5,   0, 0, 0,    9,    0, 0, 0, 0,
                               'R',  'E', 'M', 'B', 1, 7, 0xd0, 0x90, 0, 0, 0, 1};
        ASSERT_OK(bwe_on_rtcp_feedback(&bwe, cap, sizeof(cap), 0));
    }
    head = tail = feedback_counter = 0;
    uint64_t budget = 0, next_departure_us = 0;
    uint32_t sequence = UINT16_MAX - 500u;
    uint32_t down_at = UINT32_MAX, up_at = UINT32_MAX;
    uint32_t rate_bytes[LINK_RATE_STEPS] = {0};
    uint32_t rate_total = 0;
    const uint32_t clock_base = UINT32_MAX - 2000u;
    for (uint32_t elapsed = 0; elapsed <= LINK_END_MS; elapsed += LINK_STEP_MS) {
        uint32_t now_ms = clock_base + elapsed;
        uint32_t target = bwe_get_bitrate(&bwe);
        if (fixed_cap)
            ASSERT_TRUE(target <= 500000u);
        uint32_t capacity = elapsed >= LINK_DOWN_MS && elapsed < LINK_UP_MS ? 300000u : 2000000u;
        unsigned rate_slot = (elapsed / LINK_STEP_MS) % LINK_RATE_STEPS;
        rate_total -= rate_bytes[rate_slot];
        rate_bytes[rate_slot] = 0;
        budget += (uint64_t)target * LINK_STEP_MS;
        while (budget >= LINK_PACKET_BYTES * 8000u) {
            budget -= LINK_PACKET_BYTES * 8000u;
            rate_total += LINK_PACKET_BYTES;
            rate_bytes[rate_slot] += LINK_PACKET_BYTES;
            ASSERT_TRUE(tail - head < LINK_QUEUE_SIZE);
            link_packet_t *packet = &queue[tail++ % LINK_QUEUE_SIZE];
            packet->sequence = sequence++;
            packet->sent_ms = now_ms;
            bwe_record_transmit(&bwe, packet->sequence, LINK_PACKET_BYTES, now_ms, true, false);
            uint64_t send_us = (uint64_t)elapsed * 1000u;
            if (next_departure_us < send_us)
                next_departure_us = send_us;
            packet->lost = next_departure_us - send_us > 40000u;
            if (!packet->lost) {
                next_departure_us += (uint64_t)LINK_PACKET_BYTES * 8000000u / capacity;
                packet->arrival_us = next_departure_us + 25000u;
            }
        }
        if (elapsed && elapsed % feedback_ms == 0)
            feedback(now_ms, elapsed);
        uint64_t send_bps = (uint64_t)rate_total * 8u;
        if (elapsed >= LINK_DOWN_MS && elapsed < LINK_UP_MS && send_bps <= 360000u &&
            bwe_get_bitrate(&bwe) <= 360000u && down_at == UINT32_MAX)
            down_at = elapsed - LINK_DOWN_MS;
        if (!fixed_cap && elapsed >= LINK_UP_MS && send_bps >= 1600000u && up_at == UINT32_MAX)
            up_at = elapsed - LINK_UP_MS;
    }
    printf("feedback=%ums cap=%d decrease=%ums recovery=%ums final=%u\n", feedback_ms, fixed_cap,
           down_at, up_at, bwe_get_bitrate(&bwe));
    ASSERT_TRUE(down_at <= 2000u);
    if (!fixed_cap) {
        ASSERT_TRUE(up_at <= 15000u);
        ASSERT_TRUE(bwe_get_bitrate(&bwe) >= 1600000u);
    }
}

TEST(test_network_throttle_and_recovery_independent_of_feedback_period)
{
    simulate(50, false);
    simulate(100, false);
    simulate(200, false);
}
TEST(test_network_respects_persistent_receiver_cap)
{
    simulate(100, true);
}
TEST(test_zero_receiver_cap_can_resume_and_notify)
{
    ASSERT_OK(bwe_init(&bwe));
    uint8_t cap[] = {0x8f, 206, 0,   5,   0, 0, 0, 9, 0, 0, 0, 0,
                     'R',  'E', 'M', 'B', 1, 0, 0, 0, 0, 0, 0, 1};
    ASSERT_OK(bwe_on_rtcp_feedback(&bwe, cap, sizeof(cap), 100));
    ASSERT_EQ(bwe_get_bitrate(&bwe), 0);
    ASSERT_TRUE(bwe_should_emit_event(&bwe));
    cap[17] = 7;
    cap[18] = 0xd0;
    cap[19] = 0x90;
    ASSERT_OK(bwe_on_rtcp_feedback(&bwe, cap, sizeof(cap), 200));
    ASSERT_EQ(bwe_get_bitrate(&bwe), 500000);
    ASSERT_TRUE(bwe_should_emit_event(&bwe));
    ASSERT_FALSE(bwe_should_emit_event(&bwe));
}

/* Positive control plus separate load/delay/failure/history/retransmit gates. */
static void probe_gate(unsigned mode)
{
    ASSERT_OK(bwe_init(&bwe));
    head = tail = feedback_counter = 0;
    for (uint32_t elapsed = 0; elapsed < 4000; elapsed += LINK_STEP_MS) {
        ASSERT_TRUE(tail - head < LINK_QUEUE_SIZE);
        link_packet_t *packet = &queue[tail++ % LINK_QUEUE_SIZE];
        packet->sequence = elapsed / LINK_STEP_MS;
        packet->sent_ms = elapsed;
        packet->lost = false;
        packet->arrival_us = elapsed * 1000u + 25000u + (mode == 2 ? elapsed * 250u : 0u);
        bwe_record_transmit(&bwe, packet->sequence, mode == 1 ? 20 : LINK_PACKET_BYTES, elapsed,
                            mode != 4 || packet->sequence % 4u != 0, false);
        if (mode == 3)
            bwe_record_drop(&bwe, elapsed);
        if (mode == 5)
            bwe_record_transmit(&bwe, packet->sequence, LINK_PACKET_BYTES, elapsed, true, true);
        if (elapsed && elapsed % 100u == 0)
            feedback(elapsed, elapsed);
    }
    if (mode == 0)
        ASSERT_TRUE(bwe_get_bitrate(&bwe) > NANORTC_BWE_INITIAL_BITRATE);
    else
        ASSERT_EQ(bwe_get_bitrate(&bwe), NANORTC_BWE_INITIAL_BITRATE);
    if (mode == 4)
        ASSERT_TRUE(bwe.history_missed > 0);
}

TEST(test_upward_probing_requires_load_delay_history_and_success)
{
    for (unsigned mode = 0; mode < 6; mode++)
        probe_gate(mode);
}
#endif
TEST_MAIN_BEGIN("BWE finite-queue network")
#if NANORTC_FEATURE_VIDEO
RUN(test_network_throttle_and_recovery_independent_of_feedback_period);
RUN(test_network_respects_persistent_receiver_cap);
RUN(test_zero_receiver_cap_can_resume_and_notify);
RUN(test_upward_probing_requires_load_delay_history_and_success);
#endif
TEST_MAIN_END
