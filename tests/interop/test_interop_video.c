/*
 * nanortc interop tests — Video interoperability with libdatachannel
 *
 * Test topology:
 *   libdatachannel (offerer/controlling) <--localhost UDP--> nanortc (answerer/controlled)
 *
 * Verifies H.264 video track negotiation, SRTP-protected RTP transport,
 * FU-A fragmentation/reassembly, and keyframe detection.
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
static uint16_t next_port = INTEROP_PORT_BASE + 200; /* offset from DC/audio tests */

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
 * Full setup for video interop: create signaling pipe, start both peers
 * with video tracks, wait for connection.
 */
static int setup_video_pair(interop_sig_pipe_t *pipe, interop_nanortc_media_peer_t *nano,
                            interop_libdatachannel_media_peer_t *ldc, nanortc_direction_t nano_dir,
                            ldc_direction_t ldc_dir)
{
    uint16_t port = alloc_port();

    if (interop_sig_create(pipe) != 0) {
        fprintf(stderr, "[test] Failed to create signaling pipe\n");
        return -1;
    }

    /* Configure nanortc video track */
    interop_media_track_config_t nano_track;
    memset(&nano_track, 0, sizeof(nano_track));
    nano_track.kind = NANORTC_TRACK_VIDEO;
    nano_track.direction = nano_dir;
    nano_track.codec = NANORTC_CODEC_H264;
    nano_track.sample_rate = 0;
    nano_track.channels = 0;

    /* Start nanortc peer first (it waits for offer) */
    if (interop_nanortc_media_start(nano, pipe->fd[0], port, &nano_track, 1) != 0) {
        fprintf(stderr, "[test] Failed to start nanortc media peer\n");
        interop_sig_destroy(pipe);
        return -1;
    }

    /* Configure libdatachannel video track */
    ldc_track_config_t ldc_track;
    memset(&ldc_track, 0, sizeof(ldc_track));
    ldc_track.kind = LDC_TRACK_VIDEO;
    ldc_track.direction = ldc_dir;
    ldc_track.codec = LDC_CODEC_H264;
    ldc_track.ssrc = 2001;
    ldc_track.payload_type = 96;

    /* Start libdatachannel peer */
    if (interop_libdatachannel_media_start(ldc, pipe->fd[1], &ldc_track, 1, port) != 0) {
        fprintf(stderr, "[test] Failed to start libdatachannel media peer\n");
        interop_nanortc_media_stop(nano);
        interop_sig_destroy(pipe);
        return -1;
    }

    return 0;
}

static void teardown_video_pair(interop_sig_pipe_t *pipe, interop_nanortc_media_peer_t *nano,
                                interop_libdatachannel_media_peer_t *ldc)
{
    interop_libdatachannel_media_stop(ldc);
    interop_nanortc_media_stop(nano);
    interop_sig_destroy(pipe);
}

/*
 * Build a minimal H.264 Annex-B access unit.
 * Annex-B format: 0x00 0x00 0x00 0x01 <NAL header> <payload>
 *
 * NAL type is encoded in the low 5 bits of the NAL header byte.
 *   IDR slice (keyframe) = 0x65 (nal_type=5, nri=3)
 *   Non-IDR slice        = 0x41 (nal_type=1, nri=2)
 */
static void build_h264_frame(uint8_t *buf, size_t len, bool keyframe, uint8_t fill)
{
    /* Annex-B start code */
    buf[0] = 0x00;
    buf[1] = 0x00;
    buf[2] = 0x00;
    buf[3] = 0x01;
    /* NAL header */
    buf[4] = keyframe ? 0x65 : 0x41;
    /* Payload */
    for (size_t i = 5; i < len; i++) {
        buf[i] = fill;
    }
}

/* ----------------------------------------------------------------
 * Test: Video handshake (ICE + DTLS with video m-line)
 * ---------------------------------------------------------------- */

TEST(test_interop_video_handshake)
{
    interop_sig_pipe_t pipe;
    interop_nanortc_media_peer_t nano;
    interop_libdatachannel_media_peer_t ldc;

    int rc = setup_video_pair(&pipe, &nano, &ldc, NANORTC_DIR_RECVONLY, LDC_DIR_SENDONLY);
    ASSERT_OK(rc);

    /* Wait for connection on both sides */
    rc = interop_libdatachannel_media_wait_flag(&ldc.connected, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    rc = interop_libdatachannel_media_wait_flag((atomic_int *)&nano.connected, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    /* Verify nanortc passed through connection stages */
    ASSERT_TRUE(atomic_load(&nano.ice_connected));
    ASSERT_TRUE(atomic_load(&nano.dtls_connected));

    teardown_video_pair(&pipe, &nano, &ldc);
}

/* ----------------------------------------------------------------
 * Test: H.264 video from libdatachannel to nanortc
 * ---------------------------------------------------------------- */

TEST(test_interop_video_h264_ldc_to_nano)
{
    interop_sig_pipe_t pipe;
    interop_nanortc_media_peer_t nano;
    interop_libdatachannel_media_peer_t ldc;

    int rc = setup_video_pair(&pipe, &nano, &ldc, NANORTC_DIR_RECVONLY, LDC_DIR_SENDONLY);
    ASSERT_OK(rc);

    /* Wait for track to be ready */
    rc = interop_libdatachannel_media_wait_flag(&ldc.connected, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_libdatachannel_media_wait_flag(&ldc.track_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_libdatachannel_media_wait_flag((atomic_int *)&nano.connected, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    /* Send a small H.264 keyframe (fits in single RTP packet, no FU-A) */
    uint8_t h264_frame[64];
    build_h264_frame(h264_frame, sizeof(h264_frame), true, 0xAA);

    int initial_count = atomic_load(&nano.frame_count);
    rc = interop_libdatachannel_media_send(&ldc, 0, h264_frame, sizeof(h264_frame));
    ASSERT_TRUE(rc >= 0);

    /* Wait for nanortc to receive */
    rc = wait_frame_count(&nano.frame_count, initial_count + 1, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    /* Verify frame was received */
    interop_media_frame_t received;
    rc = interop_nanortc_media_get_last_frame(&nano, &received);
    ASSERT_OK(rc);
    ASSERT_TRUE(received.len > 0);

    teardown_video_pair(&pipe, &nano, &ldc);
}

/* ----------------------------------------------------------------
 * Test: H.264 video from nanortc to libdatachannel
 * ---------------------------------------------------------------- */

TEST(test_interop_video_h264_nano_to_ldc)
{
    interop_sig_pipe_t pipe;
    interop_nanortc_media_peer_t nano;
    interop_libdatachannel_media_peer_t ldc;

    int rc = setup_video_pair(&pipe, &nano, &ldc, NANORTC_DIR_SENDONLY, LDC_DIR_RECVONLY);
    ASSERT_OK(rc);

    /* Wait for connection */
    rc = interop_libdatachannel_media_wait_flag(&ldc.connected, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_libdatachannel_media_wait_flag((atomic_int *)&nano.connected, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    /* Give SRTP time to set up after DTLS */
    interop_sleep_ms(200);

    /* Send H.264 keyframe from nanortc (Annex-B format) */
    uint8_t h264_frame[64];
    build_h264_frame(h264_frame, sizeof(h264_frame), true, 0xBB);

    int initial_count = atomic_load(&ldc.frame_count);
    ASSERT_TRUE(nano.track_mids[0] >= 0);
    rc = interop_nanortc_media_send_video(&nano, (uint8_t)nano.track_mids[0], nano_get_millis(),
                                          h264_frame, sizeof(h264_frame));
    ASSERT_OK(rc);

    /* Wait for libdatachannel to receive */
    rc = wait_frame_count(&ldc.frame_count, initial_count + 1, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    /* Verify libdatachannel received data */
    interop_media_frame_t received;
    rc = interop_libdatachannel_media_get_last_frame(&ldc, &received);
    ASSERT_OK(rc);
    ASSERT_TRUE(received.len > 0);

    teardown_video_pair(&pipe, &nano, &ldc);
}

/* ----------------------------------------------------------------
 * Test: Keyframe detection
 * ---------------------------------------------------------------- */

TEST(test_interop_video_keyframe)
{
    interop_sig_pipe_t pipe;
    interop_nanortc_media_peer_t nano;
    interop_libdatachannel_media_peer_t ldc;

    int rc = setup_video_pair(&pipe, &nano, &ldc, NANORTC_DIR_RECVONLY, LDC_DIR_SENDONLY);
    ASSERT_OK(rc);

    /* Wait for track to be ready */
    rc = interop_libdatachannel_media_wait_flag(&ldc.connected, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_libdatachannel_media_wait_flag(&ldc.track_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_libdatachannel_media_wait_flag((atomic_int *)&nano.connected, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    /* Send an IDR keyframe */
    uint8_t keyframe[64];
    build_h264_frame(keyframe, sizeof(keyframe), true, 0xCC);

    int initial_count = atomic_load(&nano.frame_count);
    rc = interop_libdatachannel_media_send(&ldc, 0, keyframe, sizeof(keyframe));
    ASSERT_TRUE(rc >= 0);

    /* Wait for reception */
    rc = wait_frame_count(&nano.frame_count, initial_count + 1, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    /* Verify keyframe flag is set */
    interop_media_frame_t received;
    rc = interop_nanortc_media_get_last_frame(&nano, &received);
    ASSERT_OK(rc);
    ASSERT_TRUE(received.is_keyframe);

    /* Now send a non-IDR frame */
    uint8_t non_idr[64];
    build_h264_frame(non_idr, sizeof(non_idr), false, 0xDD);

    initial_count = atomic_load(&nano.frame_count);
    rc = interop_libdatachannel_media_send(&ldc, 0, non_idr, sizeof(non_idr));
    ASSERT_TRUE(rc >= 0);

    rc = wait_frame_count(&nano.frame_count, initial_count + 1, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    rc = interop_nanortc_media_get_last_frame(&nano, &received);
    ASSERT_OK(rc);
    ASSERT_FALSE(received.is_keyframe);

    teardown_video_pair(&pipe, &nano, &ldc);
}

/* ----------------------------------------------------------------
 * Test: FU-A fragmentation (large NAL > MTU)
 * ---------------------------------------------------------------- */

TEST(test_interop_video_fua_fragmentation)
{
    interop_sig_pipe_t pipe;
    interop_nanortc_media_peer_t nano;
    interop_libdatachannel_media_peer_t ldc;

    int rc = setup_video_pair(&pipe, &nano, &ldc, NANORTC_DIR_RECVONLY, LDC_DIR_SENDONLY);
    ASSERT_OK(rc);

    /* Wait for track to be ready */
    rc = interop_libdatachannel_media_wait_flag(&ldc.connected, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_libdatachannel_media_wait_flag(&ldc.track_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_libdatachannel_media_wait_flag((atomic_int *)&nano.connected, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    /*
     * Send a large H.264 frame (1800 bytes > 1200 MTU).
     * This forces FU-A fragmentation on the sender and reassembly on nanortc.
     */
    uint8_t large_frame[1800];
    build_h264_frame(large_frame, sizeof(large_frame), true, 0xEE);

    int initial_count = atomic_load(&nano.frame_count);
    rc = interop_libdatachannel_media_send(&ldc, 0, large_frame, sizeof(large_frame));
    ASSERT_TRUE(rc >= 0);

    /* Wait for nanortc to reassemble and deliver the frame */
    rc = wait_frame_count(&nano.frame_count, initial_count + 1, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    /* Verify the reassembled frame is present */
    interop_media_frame_t received;
    rc = interop_nanortc_media_get_last_frame(&nano, &received);
    ASSERT_OK(rc);
    ASSERT_TRUE(received.len > 0);
    ASSERT_TRUE(received.is_keyframe);

    teardown_video_pair(&pipe, &nano, &ldc);
}

/* ----------------------------------------------------------------
 * Test: Audio + Video combined (two tracks simultaneously)
 * ---------------------------------------------------------------- */

TEST(test_interop_audio_video_combined)
{
    uint16_t port = alloc_port();
    interop_sig_pipe_t pipe;
    interop_nanortc_media_peer_t nano;
    interop_libdatachannel_media_peer_t ldc;

    if (interop_sig_create(&pipe) != 0) {
        ASSERT_TRUE(0); /* fail */
    }

    /* Configure nanortc with both audio and video tracks */
    interop_media_track_config_t nano_tracks[2];
    memset(nano_tracks, 0, sizeof(nano_tracks));

    nano_tracks[0].kind = NANORTC_TRACK_AUDIO;
    nano_tracks[0].direction = NANORTC_DIR_RECVONLY;
    nano_tracks[0].codec = NANORTC_CODEC_OPUS;
    nano_tracks[0].sample_rate = 48000;
    nano_tracks[0].channels = 2;

    nano_tracks[1].kind = NANORTC_TRACK_VIDEO;
    nano_tracks[1].direction = NANORTC_DIR_RECVONLY;
    nano_tracks[1].codec = NANORTC_CODEC_H264;

    int rc = interop_nanortc_media_start(&nano, pipe.fd[0], port, nano_tracks, 2);
    ASSERT_OK(rc);

    /* Configure libdatachannel with both tracks */
    ldc_track_config_t ldc_tracks[2];
    memset(ldc_tracks, 0, sizeof(ldc_tracks));

    ldc_tracks[0].kind = LDC_TRACK_AUDIO;
    ldc_tracks[0].direction = LDC_DIR_SENDONLY;
    ldc_tracks[0].codec = LDC_CODEC_OPUS;
    ldc_tracks[0].ssrc = 3001;
    ldc_tracks[0].payload_type = 111;

    ldc_tracks[1].kind = LDC_TRACK_VIDEO;
    ldc_tracks[1].direction = LDC_DIR_SENDONLY;
    ldc_tracks[1].codec = LDC_CODEC_H264;
    ldc_tracks[1].ssrc = 3002;
    ldc_tracks[1].payload_type = 96;

    rc = interop_libdatachannel_media_start(&ldc, pipe.fd[1], ldc_tracks, 2, port);
    ASSERT_OK(rc);

    /* Wait for connection */
    rc = interop_libdatachannel_media_wait_flag(&ldc.connected, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_libdatachannel_media_wait_flag(&ldc.track_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_libdatachannel_media_wait_flag((atomic_int *)&nano.connected, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    /* Send audio frame */
    uint8_t audio_frame[160];
    for (size_t i = 0; i < sizeof(audio_frame); i++) {
        audio_frame[i] = (uint8_t)(0x60 + i);
    }

    int initial_count = atomic_load(&nano.frame_count);
    rc = interop_libdatachannel_media_send(&ldc, 0, audio_frame, sizeof(audio_frame));
    ASSERT_TRUE(rc >= 0);

    /* Send video frame */
    uint8_t video_frame[64];
    build_h264_frame(video_frame, sizeof(video_frame), true, 0xFF);

    rc = interop_libdatachannel_media_send(&ldc, 1, video_frame, sizeof(video_frame));
    ASSERT_TRUE(rc >= 0);

    /* Wait for at least 2 frames (one audio + one video) */
    rc = wait_frame_count(&nano.frame_count, initial_count + 2, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    /* Verify we received frames from both tracks */
    ASSERT_TRUE(atomic_load(&nano.frame_count) >= initial_count + 2);

    interop_libdatachannel_media_stop(&ldc);
    interop_nanortc_media_stop(&nano);
    interop_sig_destroy(&pipe);
}

/* ----------------------------------------------------------------
 * Test runner
 * ---------------------------------------------------------------- */

TEST_MAIN_BEGIN("interop-video")
RUN(test_interop_video_handshake);
RUN(test_interop_video_h264_ldc_to_nano);
RUN(test_interop_video_h264_nano_to_ldc);
RUN(test_interop_video_keyframe);
RUN(test_interop_audio_video_combined);
TEST_MAIN_END
