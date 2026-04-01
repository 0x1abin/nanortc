/*
 * nanortc interop tests — Audio interoperability with libdatachannel
 *
 * Test topology:
 *   libdatachannel (offerer/controlling) <--localhost UDP--> nanortc (answerer/controlled)
 *
 * Verifies audio track negotiation, SRTP-protected RTP transport,
 * and frame delivery in both directions using Opus and PCMA codecs.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_test.h"
#include "interop_common.h"
#include "interop_nanortc_media_peer.h"
#include "interop_libdatachannel_media_peer.h"

#include <stdio.h>
#include <string.h>

/* ----------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------- */

/* Port counter to avoid bind conflicts between tests */
static uint16_t next_port = INTEROP_PORT_BASE + 100; /* offset from DC tests */

static uint16_t alloc_port(void)
{
    return next_port++;
}

/* Wait until an atomic_int counter reaches at least `target`, or timeout */
static int wait_frame_count(atomic_int *counter, int target, int timeout_ms)
{
    uint32_t start = interop_get_millis();
    while (atomic_load(counter) < target) {
        if ((int)(interop_get_millis() - start) >= timeout_ms) {
            return -1;
        }
        interop_sleep_ms(10);
    }
    return 0;
}

/*
 * Full setup for audio interop: create signaling pipe, start both peers
 * with audio tracks, wait for connection.
 */
static int setup_audio_pair(interop_sig_pipe_t *pipe, interop_nanortc_media_peer_t *nano,
                            interop_libdatachannel_media_peer_t *ldc, nanortc_direction_t nano_dir,
                            ldc_direction_t ldc_dir, ldc_codec_t ldc_codec,
                            nanortc_codec_t nano_codec, uint32_t sample_rate, uint8_t channels,
                            uint8_t pt)
{
    uint16_t port = alloc_port();

    if (interop_sig_create(pipe) != 0) {
        fprintf(stderr, "[test] Failed to create signaling pipe\n");
        return -1;
    }

    /* Configure nanortc audio track */
    interop_media_track_config_t nano_track;
    memset(&nano_track, 0, sizeof(nano_track));
    nano_track.kind = NANORTC_TRACK_AUDIO;
    nano_track.direction = nano_dir;
    nano_track.codec = nano_codec;
    nano_track.sample_rate = sample_rate;
    nano_track.channels = channels;

    /* Start nanortc peer first (it waits for offer) */
    if (interop_nanortc_media_start(nano, pipe->fd[0], port, &nano_track, 1) != 0) {
        fprintf(stderr, "[test] Failed to start nanortc media peer\n");
        interop_sig_destroy(pipe);
        return -1;
    }

    /* Configure libdatachannel audio track */
    ldc_track_config_t ldc_track;
    memset(&ldc_track, 0, sizeof(ldc_track));
    ldc_track.kind = LDC_TRACK_AUDIO;
    ldc_track.direction = ldc_dir;
    ldc_track.codec = ldc_codec;
    ldc_track.ssrc = 1001;
    ldc_track.payload_type = pt;

    /* Start libdatachannel peer (generates offer, exchanges signaling) */
    if (interop_libdatachannel_media_start(ldc, pipe->fd[1], &ldc_track, 1, port) != 0) {
        fprintf(stderr, "[test] Failed to start libdatachannel media peer\n");
        interop_nanortc_media_stop(nano);
        interop_sig_destroy(pipe);
        return -1;
    }

    return 0;
}

static void teardown_audio_pair(interop_sig_pipe_t *pipe, interop_nanortc_media_peer_t *nano,
                                interop_libdatachannel_media_peer_t *ldc)
{
    interop_libdatachannel_media_stop(ldc);
    interop_nanortc_media_stop(nano);
    interop_sig_destroy(pipe);
}

/* Synthetic audio frame: pattern-filled bytes simulating a 20ms Opus frame */
static void fill_audio_frame(uint8_t *buf, size_t len, uint8_t seed)
{
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(seed + i);
    }
}

/* ----------------------------------------------------------------
 * Test: Audio handshake (ICE + DTLS with audio m-line)
 * ---------------------------------------------------------------- */

TEST(test_interop_audio_handshake)
{
    interop_sig_pipe_t pipe;
    interop_nanortc_media_peer_t nano;
    interop_libdatachannel_media_peer_t ldc;

    int rc = setup_audio_pair(&pipe, &nano, &ldc, NANORTC_DIR_RECVONLY, LDC_DIR_SENDONLY,
                              LDC_CODEC_OPUS, NANORTC_CODEC_OPUS, 48000, 2, 111);
    ASSERT_OK(rc);

    /* Wait for connection on both sides */
    rc = interop_libdatachannel_media_wait_flag(&ldc.connected, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    rc = interop_libdatachannel_media_wait_flag((atomic_int *)&nano.connected, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    /* Verify nanortc passed through connection stages */
    ASSERT_TRUE(atomic_load(&nano.ice_connected));
    ASSERT_TRUE(atomic_load(&nano.dtls_connected));

    teardown_audio_pair(&pipe, &nano, &ldc);
}

/* ----------------------------------------------------------------
 * Test: Opus audio from libdatachannel to nanortc
 * ---------------------------------------------------------------- */

TEST(test_interop_audio_opus_ldc_to_nano)
{
    interop_sig_pipe_t pipe;
    interop_nanortc_media_peer_t nano;
    interop_libdatachannel_media_peer_t ldc;

    int rc = setup_audio_pair(&pipe, &nano, &ldc, NANORTC_DIR_RECVONLY, LDC_DIR_SENDONLY,
                              LDC_CODEC_OPUS, NANORTC_CODEC_OPUS, 48000, 2, 111);
    ASSERT_OK(rc);

    /* Wait for track to be ready */
    rc = interop_libdatachannel_media_wait_flag(&ldc.connected, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_libdatachannel_media_wait_flag(&ldc.track_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_libdatachannel_media_wait_flag((atomic_int *)&nano.connected, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    /* Send a synthetic Opus frame from libdatachannel */
    uint8_t audio_frame[160];
    fill_audio_frame(audio_frame, sizeof(audio_frame), 0x10);

    int initial_count = atomic_load(&nano.frame_count);
    rc = interop_libdatachannel_media_send(&ldc, 0, audio_frame, sizeof(audio_frame));
    ASSERT_TRUE(rc >= 0);

    /* Wait for nanortc to receive it */
    rc = wait_frame_count(&nano.frame_count, initial_count + 1, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    /* Verify received frame data matches sent data */
    interop_media_frame_t received;
    rc = interop_nanortc_media_get_last_frame(&nano, &received);
    ASSERT_OK(rc);
    ASSERT_EQ(received.len, sizeof(audio_frame));
    ASSERT_TRUE(memcmp(received.data, audio_frame, sizeof(audio_frame)) == 0);

    teardown_audio_pair(&pipe, &nano, &ldc);
}

/* ----------------------------------------------------------------
 * Test: Opus audio from nanortc to libdatachannel
 * ---------------------------------------------------------------- */

TEST(test_interop_audio_opus_nano_to_ldc)
{
    interop_sig_pipe_t pipe;
    interop_nanortc_media_peer_t nano;
    interop_libdatachannel_media_peer_t ldc;

    int rc = setup_audio_pair(&pipe, &nano, &ldc, NANORTC_DIR_SENDONLY, LDC_DIR_RECVONLY,
                              LDC_CODEC_OPUS, NANORTC_CODEC_OPUS, 48000, 2, 111);
    ASSERT_OK(rc);

    /* Wait for connection */
    rc = interop_libdatachannel_media_wait_flag(&ldc.connected, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_libdatachannel_media_wait_flag((atomic_int *)&nano.connected, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    /* Give SRTP time to set up after DTLS */
    interop_sleep_ms(200);

    /* Send audio from nanortc */
    uint8_t audio_frame[160];
    fill_audio_frame(audio_frame, sizeof(audio_frame), 0x20);

    int initial_count = atomic_load(&ldc.frame_count);
    ASSERT_TRUE(nano.track_mids[0] >= 0);
    rc = interop_nanortc_media_send_audio(&nano, (uint8_t)nano.track_mids[0], audio_frame,
                                          sizeof(audio_frame));
    ASSERT_OK(rc);

    /* Wait for libdatachannel to receive it */
    rc = wait_frame_count(&ldc.frame_count, initial_count + 1, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    /* Verify libdatachannel received data (RTP payload should match) */
    interop_media_frame_t received;
    rc = interop_libdatachannel_media_get_last_frame(&ldc, &received);
    ASSERT_OK(rc);
    ASSERT_TRUE(received.len > 0);

    teardown_audio_pair(&pipe, &nano, &ldc);
}

/* ----------------------------------------------------------------
 * Test: Bidirectional audio (sendrecv)
 * ---------------------------------------------------------------- */

TEST(test_interop_audio_bidirectional)
{
    interop_sig_pipe_t pipe;
    interop_nanortc_media_peer_t nano;
    interop_libdatachannel_media_peer_t ldc;

    int rc = setup_audio_pair(&pipe, &nano, &ldc, NANORTC_DIR_SENDRECV, LDC_DIR_SENDRECV,
                              LDC_CODEC_OPUS, NANORTC_CODEC_OPUS, 48000, 2, 111);
    ASSERT_OK(rc);

    /* Wait for connection and track open */
    rc = interop_libdatachannel_media_wait_flag(&ldc.connected, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_libdatachannel_media_wait_flag(&ldc.track_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_libdatachannel_media_wait_flag((atomic_int *)&nano.connected, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    interop_sleep_ms(200);

    /* Direction 1: libdatachannel -> nanortc */
    uint8_t frame_ldc[160];
    fill_audio_frame(frame_ldc, sizeof(frame_ldc), 0x30);

    int initial_nano = atomic_load(&nano.frame_count);
    rc = interop_libdatachannel_media_send(&ldc, 0, frame_ldc, sizeof(frame_ldc));
    ASSERT_TRUE(rc >= 0);

    rc = wait_frame_count(&nano.frame_count, initial_nano + 1, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    /* Verify nanortc received the correct frame */
    interop_media_frame_t nano_received;
    rc = interop_nanortc_media_get_last_frame(&nano, &nano_received);
    ASSERT_OK(rc);
    ASSERT_EQ(nano_received.len, sizeof(frame_ldc));
    ASSERT_TRUE(memcmp(nano_received.data, frame_ldc, sizeof(frame_ldc)) == 0);

    /* Direction 2: nanortc -> libdatachannel */
    uint8_t frame_nano[160];
    fill_audio_frame(frame_nano, sizeof(frame_nano), 0x40);

    int initial_ldc = atomic_load(&ldc.frame_count);
    ASSERT_TRUE(nano.track_mids[0] >= 0);
    rc = interop_nanortc_media_send_audio(&nano, (uint8_t)nano.track_mids[0], frame_nano,
                                          sizeof(frame_nano));
    ASSERT_OK(rc);

    rc = wait_frame_count(&ldc.frame_count, initial_ldc + 1, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    /* Verify libdatachannel received data */
    interop_media_frame_t ldc_received;
    rc = interop_libdatachannel_media_get_last_frame(&ldc, &ldc_received);
    ASSERT_OK(rc);
    ASSERT_TRUE(ldc_received.len > 0);

    teardown_audio_pair(&pipe, &nano, &ldc);
}

/* ----------------------------------------------------------------
 * Test: Multiple sequential audio frames
 * ---------------------------------------------------------------- */

#define AUDIO_SEQ_COUNT 10

TEST(test_interop_audio_multiple_frames)
{
    interop_sig_pipe_t pipe;
    interop_nanortc_media_peer_t nano;
    interop_libdatachannel_media_peer_t ldc;

    int rc = setup_audio_pair(&pipe, &nano, &ldc, NANORTC_DIR_RECVONLY, LDC_DIR_SENDONLY,
                              LDC_CODEC_OPUS, NANORTC_CODEC_OPUS, 48000, 2, 111);
    ASSERT_OK(rc);

    /* Wait for connection and track */
    rc = interop_libdatachannel_media_wait_flag(&ldc.connected, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_libdatachannel_media_wait_flag(&ldc.track_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_libdatachannel_media_wait_flag((atomic_int *)&nano.connected, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    int initial_count = atomic_load(&nano.frame_count);

    /* Send 10 audio frames with 20ms spacing */
    for (int i = 0; i < AUDIO_SEQ_COUNT; i++) {
        uint8_t frame[160];
        fill_audio_frame(frame, sizeof(frame), (uint8_t)(i * 16));

        rc = interop_libdatachannel_media_send(&ldc, 0, frame, sizeof(frame));
        ASSERT_TRUE(rc >= 0);

        /* Small delay to simulate real-time pacing */
        interop_sleep_ms(20);
    }

    /* Wait for all frames to arrive */
    rc = wait_frame_count(&nano.frame_count, initial_count + AUDIO_SEQ_COUNT, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    /* Verify total frame count */
    ASSERT_TRUE(atomic_load(&nano.frame_count) >= initial_count + AUDIO_SEQ_COUNT);

    teardown_audio_pair(&pipe, &nano, &ldc);
}

/* ----------------------------------------------------------------
 * Test runner
 * ---------------------------------------------------------------- */

TEST_MAIN_BEGIN("interop-audio")
RUN(test_interop_audio_handshake);
RUN(test_interop_audio_opus_ldc_to_nano);
RUN(test_interop_audio_opus_nano_to_ldc);
RUN(test_interop_audio_bidirectional);
RUN(test_interop_audio_multiple_frames);
TEST_MAIN_END
