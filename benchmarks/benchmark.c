/* Reproducible CPU microbenchmarks; run a Release build on an idle host.
 * Reported p95 is across batch averages, not individual packet latency.
 * SPDX-License-Identifier: MIT */
#include "nanortc.h"
#include "nano_rtc_internal.h"
#include "nanortc_crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BENCH_BATCHES       11
#define BENCH_ITERATIONS    2000
#define BENCH_FRAME_BYTES   12000
#define BENCH_PAYLOAD_BYTES 1100

static nanortc_t rtc;
static uint8_t payload[BENCH_PAYLOAD_BYTES];
static uint8_t packet[NANORTC_STUN_BUF_SIZE];
static volatile uint32_t checksum;

static void check(int rc)
{
    if (rc != NANORTC_OK) {
        fprintf(stderr, "benchmark operation failed: %d\n", rc);
        exit(1);
    }
}

static void report_sent(const nanortc_output_t *out)
{
#if NANORTC_VERSION_MAJOR > 0 || NANORTC_VERSION_MINOR >= 2
    check(nanortc_report_transmit(&rtc, out->transmit.token, NANORTC_TRANSMIT_SENT, rtc.now_ms));
#else
    (void)out; /* The same workload also builds against the 0.1 baseline. */
#endif
}

static const nanortc_crypto_provider_t *crypto(void)
{
#if defined(NANORTC_CRYPTO_OPENSSL)
    return nanortc_crypto_openssl();
#else
    return nanortc_crypto_mbedtls();
#endif
}

static void measure(const char *name, void (*operation)(void))
{
    double samples[BENCH_BATCHES];
    for (unsigned i = 0; i < BENCH_ITERATIONS; i++)
        operation();
    for (unsigned batch = 0; batch < BENCH_BATCHES; batch++) {
        clock_t start = clock();
        for (unsigned i = 0; i < BENCH_ITERATIONS; i++)
            operation();
        clock_t end = clock();
        if (start == (clock_t)-1 || end == (clock_t)-1 || end <= start) {
            fprintf(stderr, "CPU timer unavailable or too coarse\n");
            exit(1);
        }
        samples[batch] = (double)(end - start) * 1e9 / CLOCKS_PER_SEC / BENCH_ITERATIONS;
    }
    for (unsigned i = 1; i < BENCH_BATCHES; i++) {
        double value = samples[i];
        unsigned j = i;
        while (j && samples[j - 1] > value) {
            samples[j] = samples[j - 1];
            j--;
        }
        samples[j] = value;
    }
    printf("%s,median_ns=%.1f,p95_batch_ns=%.1f\n", name, samples[BENCH_BATCHES / 2],
           samples[BENCH_BATCHES - 1]);
}

static void output_drain(void)
{
    const nanortc_addr_t dest = {.family = 4, .port = 5000};
    for (unsigned i = 0; i < NANORTC_TX_SLOT_COUNT; i++) {
        uint8_t *buf;
        uint8_t slot;
        check(nano_rtc_tx_slot_acquire(&rtc, &buf, &slot));
        memcpy(buf, payload, sizeof(payload));
        check(nano_rtc_tx_slot_commit(&rtc, slot, sizeof(payload), &dest, false));
    }
    for (unsigned i = 0; i < NANORTC_TX_SLOT_COUNT; i++) {
        nanortc_output_t out;
        check(nanortc_poll_output(&rtc, &out));
        if (out.type != NANORTC_OUTPUT_TRANSMIT || out.transmit.len != sizeof(payload))
            check(NANORTC_ERR_STATE);
        checksum += out.transmit.data[0] + (uint32_t)out.transmit.len;
        report_sent(&out);
    }
}

#if NANORTC_HAVE_MEDIA_TRANSPORT
static nano_rtp_t rtp;
static nano_srtp_t srtp;

static void srtp_protect(void)
{
    size_t len, protected_len;
    check(rtp_pack(&rtp, 9000, payload, sizeof(payload), packet, sizeof(packet), &len));
    check(nano_srtp_protect(&srtp, packet, len, &protected_len));
    if (protected_len != len + NANORTC_SRTP_AUTH_TAG_SIZE)
        check(NANORTC_ERR_STATE);
    checksum += packet[protected_len - 1];
}

static void init_srtp(nano_srtp_t *state)
{
    uint8_t keys[2 * (NANORTC_SRTP_KEY_SIZE + NANORTC_SRTP_SALT_SIZE)];
    for (size_t i = 0; i < sizeof(keys); i++)
        keys[i] = (uint8_t)i;
    check(nano_srtp_init(state, crypto(), 1));
    check(nano_srtp_derive_keys(state, keys, sizeof(keys)));
}
#endif

#if NANORTC_FEATURE_VIDEO
static uint8_t frame[BENCH_FRAME_BYTES];
static uint8_t video_mid;

static void video_frame(void)
{
    rtc.now_ms += 50;
    check(nanortc_send_video(&rtc, video_mid, rtc.now_ms, frame, sizeof(frame)));
    rtc.now_ms += NANORTC_PACING_MAX_QUEUE_MS + 1;
    nanortc_output_t out;
    unsigned packets = 0;
    int rc;
    while ((rc = nanortc_poll_output(&rtc, &out)) == NANORTC_OK) {
        if (out.type == NANORTC_OUTPUT_TRANSMIT) {
            checksum += (uint32_t)out.transmit.len + out.transmit.data[0];
            packets++;
            report_sent(&out);
        }
    }
    if (rc != NANORTC_ERR_NO_DATA || packets == 0)
        check(NANORTC_ERR_STATE);
}

static void measure_video(nanortc_codec_t codec, const char *name)
{
    memset(&rtc, 0, sizeof(rtc));
    rtc.config.crypto = crypto();
    rtc.remote_addr.family = 4;
    rtc.remote_addr.port = 5000;
    rtc.bwe.estimated_bitrate = 2000000;
    init_srtp(&rtc.srtp);
    int mid = nanortc_add_video_track(&rtc, NANORTC_DIR_SENDONLY, codec);
    if (mid < 0)
        check(mid);
    video_mid = (uint8_t)mid;
    rtc.media[0].rtp.twcc_ext_id = 3;
    rtc.state = NANORTC_STATE_DTLS_CONNECTED;
    memset(frame, 0x55, sizeof(frame));
    /* Two parameter NALs followed by a large slice exercise AU lookahead/AP. */
    for (unsigned i = 0; i < 3; i++) {
        unsigned offset = i * 36;
        frame[offset] = frame[offset + 1] = frame[offset + 2] = 0;
        frame[offset + 3] = 1;
        const uint8_t h264_types[] = {0x67, 0x68, 0x65};
        const uint8_t h265_types[] = {0x40, 0x42, 0x26};
        frame[offset + 4] = codec == NANORTC_CODEC_H264 ? h264_types[i] : h265_types[i];
        frame[offset + 5] = 1;
    }
    measure(name, video_frame);
}
#endif

#if NANORTC_FEATURE_DATACHANNEL
static uint32_t tsn;
static void sctp_packet(void)
{
    size_t len = nsctp_encode_header(packet, 5000, 5000, 0x12345678);
    size_t chunk =
        nsctp_encode_data(packet + len, sizeof(packet) - len, tsn++, 0, 0, 53,
                          SCTP_DATA_FLAG_BEGIN | SCTP_DATA_FLAG_END, payload, sizeof(payload));
    if (chunk == 0)
        check(NANORTC_ERR_STATE);
    len += chunk;
    nsctp_finalize_checksum(packet, len);
    check(nsctp_verify_checksum(packet, len));
    checksum += (uint32_t)len + packet[8];
}
#endif

int main(void)
{
    memset(payload, 0x42, sizeof(payload));
#if defined(NANORTC_CRYPTO_OPENSSL)
    const char *crypto_name = "OpenSSL";
#else
    const char *crypto_name = "MbedTLS";
#endif
    printf("compiler=%s,DC=%d,AUDIO=%d,VIDEO=%d,H265=%d,FEC=%d,state_bytes=%zu\n", __VERSION__,
           NANORTC_FEATURE_DATACHANNEL, NANORTC_FEATURE_AUDIO, NANORTC_FEATURE_VIDEO,
           NANORTC_FEATURE_H265, NANORTC_FEATURE_VIDEO_FEC, sizeof(rtc));
    printf("crypto=%s,video_twcc_ext_id=3\n", crypto_name);
    printf(
        "timer=process_cpu,batches=%d,iterations=%d,frame_bytes=%d,payload_bytes=%d,tx_slots=%d\n",
        BENCH_BATCHES, BENCH_ITERATIONS, BENCH_FRAME_BYTES, BENCH_PAYLOAD_BYTES,
        NANORTC_TX_SLOT_COUNT);
    measure("output_copy_enqueue_drain_batch", output_drain);
#if NANORTC_HAVE_MEDIA_TRANSPORT
    check(rtp_init(&rtp, 0x12345678, 96));
    init_srtp(&srtp);
    measure("rtp_pack_srtp_protect", srtp_protect);
#endif
#if NANORTC_FEATURE_DATACHANNEL
    measure("sctp_encode_crc_verify", sctp_packet);
#endif
#if NANORTC_FEATURE_VIDEO
    measure_video(NANORTC_CODEC_H264, "h264_au_srtp_drain");
#if NANORTC_FEATURE_H265
    measure_video(NANORTC_CODEC_H265, "h265_au_srtp_drain");
#endif
#endif
    printf("checksum=%u\n", (unsigned)checksum);
    return 0;
}
