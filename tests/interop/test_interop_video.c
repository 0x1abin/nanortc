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
 * Optional H.265 parameter sets the nanortc peer should publish via
 * nanortc_video_set_h265_parameter_sets() (RFC 7798 §7.1). NULL fields skip
 * the publish step. Bytes are caller-owned and must outlive setup_video_pair.
 */
typedef struct {
    const uint8_t *vps;
    size_t vps_len;
    const uint8_t *sps;
    size_t sps_len;
    const uint8_t *pps;
    size_t pps_len;
} video_h265_param_sets_t;

/*
 * Full setup for video interop: create signaling pipe, start both peers
 * with video tracks, wait for connection.
 *
 * Codec-parameterized form. nano_to_ldc tests can pass `h265_sets` to publish
 * sprop-vps/sps/pps on the answer; pass NULL otherwise.
 */
static int setup_video_pair_codec(interop_sig_pipe_t *pipe, interop_nanortc_media_peer_t *nano,
                                  interop_libdatachannel_media_peer_t *ldc,
                                  nanortc_direction_t nano_dir, ldc_direction_t ldc_dir,
                                  nanortc_codec_t nano_codec, ldc_codec_t ldc_codec,
                                  uint8_t payload_type, const video_h265_param_sets_t *h265_sets)
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
    nano_track.codec = nano_codec;
    nano_track.sample_rate = 0;
    nano_track.channels = 0;
    if (h265_sets != NULL) {
        nano_track.h265_vps = h265_sets->vps;
        nano_track.h265_vps_len = h265_sets->vps_len;
        nano_track.h265_sps = h265_sets->sps;
        nano_track.h265_sps_len = h265_sets->sps_len;
        nano_track.h265_pps = h265_sets->pps;
        nano_track.h265_pps_len = h265_sets->pps_len;
    }

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
    ldc_track.codec = ldc_codec;
    ldc_track.ssrc = 2001;
    ldc_track.payload_type = payload_type;

    /* Start libdatachannel peer */
    if (interop_libdatachannel_media_start(ldc, pipe->fd[1], &ldc_track, 1, port) != 0) {
        fprintf(stderr, "[test] Failed to start libdatachannel media peer\n");
        interop_nanortc_media_stop(nano);
        interop_sig_destroy(pipe);
        return -1;
    }

    return 0;
}

/* H.264 default — preserves byte-identical behavior of existing call sites. */
static int setup_video_pair(interop_sig_pipe_t *pipe, interop_nanortc_media_peer_t *nano,
                            interop_libdatachannel_media_peer_t *ldc, nanortc_direction_t nano_dir,
                            ldc_direction_t ldc_dir)
{
    return setup_video_pair_codec(pipe, nano, ldc, nano_dir, ldc_dir, NANORTC_CODEC_H264,
                                  LDC_CODEC_H264, 96, NULL);
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

#if NANORTC_FEATURE_H265

/*
 * H.265 (HEVC) parameter sets extracted from
 * examples/sample_data/h265SampleFrames/frame-0001.h265 (1280x720, x265 r3.5+1).
 * Bytes are the NAL contents (NAL header + RBSP), no Annex-B start code.
 *
 * NAL types per RFC 7798 §1.1.4 / H.265 §7.4.2.2:
 *   VPS = 32 — header byte0 = (32 << 1) = 0x40
 *   SPS = 33 — header byte0 = (33 << 1) = 0x42
 *   PPS = 34 — header byte0 = (34 << 1) = 0x44
 */
static const uint8_t kSampleVPS[] = {
    0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00,
    0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x5d, 0xba, 0x02, 0x40,
};
static const uint8_t kSampleSPS[] = {
    0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
    0x00, 0x5d, 0xa0, 0x02, 0x80, 0x80, 0x2d, 0x16, 0x5b, 0xa9, 0x24, 0xca, 0xff, 0xf0, 0x00, 0x10,
    0x00, 0x16, 0xa0, 0x20, 0x20, 0x20, 0x80, 0x00, 0x00, 0x03, 0x00, 0x80, 0x00, 0x00, 0x0c, 0x84,
};
static const uint8_t kSamplePPS[] = {
    0x44, 0x01, 0xc1, 0x72, 0xb4, 0x62, 0x40,
};

static const video_h265_param_sets_t kSampleH265Sets = {
    .vps = kSampleVPS,
    .vps_len = sizeof(kSampleVPS),
    .sps = kSampleSPS,
    .sps_len = sizeof(kSampleSPS),
    .pps = kSamplePPS,
    .pps_len = sizeof(kSamplePPS),
};

/*
 * Build a minimal H.265 Annex-B access unit with a single VCL NAL.
 * Annex-B format: 0x00 0x00 0x00 0x01 <2-byte NAL header> <payload>
 *
 * Per RFC 7798 §1.1.4 the NAL header is 2 bytes:
 *   F(1) | nal_unit_type(6) | nuh_layer_id(6) | nuh_temporal_id_plus1(3)
 *
 * NAL type per H.265 §7.4.2.2:
 *   IDR_W_RADL = 19  (keyframe, IRAP — RFC 7798 §1.1.4)
 *   TRAIL_R    = 1   (non-key, reference)
 *
 * For TID=0 the second byte is 0x01 (nuh_temporal_id_plus1=1, layer_id=0).
 */
static void build_h265_frame(uint8_t *buf, size_t len, bool keyframe, uint8_t fill)
{
    /* Annex-B start code */
    buf[0] = 0x00;
    buf[1] = 0x00;
    buf[2] = 0x00;
    buf[3] = 0x01;
    /* 2-byte NAL header */
    if (keyframe) {
        buf[4] = (uint8_t)(19 << 1); /* IDR_W_RADL */
    } else {
        buf[4] = (uint8_t)(1 << 1); /* TRAIL_R */
    }
    buf[5] = 0x01;
    /* Payload */
    for (size_t i = 6; i < len; i++) {
        buf[i] = fill;
    }
}

/* ----------------------------------------------------------------
 * Test: H.265 video from libdatachannel to nanortc
 *
 * RFC 7798 §4.4.1 Single NAL Unit Packet — small IDR fits in one RTP packet.
 * ---------------------------------------------------------------- */

TEST(test_interop_video_h265_ldc_to_nano)
{
    interop_sig_pipe_t pipe;
    interop_nanortc_media_peer_t nano;
    interop_libdatachannel_media_peer_t ldc;

    int rc = setup_video_pair_codec(&pipe, &nano, &ldc, NANORTC_DIR_RECVONLY, LDC_DIR_SENDONLY,
                                    NANORTC_CODEC_H265, LDC_CODEC_H265, 98, NULL);
    ASSERT_OK(rc);

    rc = interop_libdatachannel_media_wait_flag(&ldc.connected, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_libdatachannel_media_wait_flag(&ldc.track_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_libdatachannel_media_wait_flag((atomic_int *)&nano.connected, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    /* Small IDR access unit — Single NAL Unit Packet path. */
    uint8_t h265_frame[64];
    build_h265_frame(h265_frame, sizeof(h265_frame), true, 0xAA);

    int initial_count = atomic_load(&nano.frame_count);
    rc = interop_libdatachannel_media_send(&ldc, 0, h265_frame, sizeof(h265_frame));
    ASSERT_TRUE(rc >= 0);

    rc = wait_frame_count(&nano.frame_count, initial_count + 1, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    interop_media_frame_t received;
    rc = interop_nanortc_media_get_last_frame(&nano, &received);
    ASSERT_OK(rc);
    ASSERT_TRUE(received.len > 0);
    ASSERT_TRUE(received.is_keyframe);

    teardown_video_pair(&pipe, &nano, &ldc);
}

/* ----------------------------------------------------------------
 * Test: H.265 video from nanortc to libdatachannel, with sprop-* fmtp
 *
 * The nanortc answer carries sprop-vps/sps/pps (RFC 7798 §7.1) extracted
 * from a real x265 stream so libdatachannel's decoder receives the
 * full parameter context.
 * ---------------------------------------------------------------- */

TEST(test_interop_video_h265_nano_to_ldc)
{
    interop_sig_pipe_t pipe;
    interop_nanortc_media_peer_t nano;
    interop_libdatachannel_media_peer_t ldc;

    int rc = setup_video_pair_codec(&pipe, &nano, &ldc, NANORTC_DIR_SENDONLY, LDC_DIR_RECVONLY,
                                    NANORTC_CODEC_H265, LDC_CODEC_H265, 98, &kSampleH265Sets);
    ASSERT_OK(rc);

    rc = interop_libdatachannel_media_wait_flag(&ldc.connected, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_libdatachannel_media_wait_flag((atomic_int *)&nano.connected, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    /* SRTP setup latency. */
    interop_sleep_ms(200);

    uint8_t h265_frame[64];
    build_h265_frame(h265_frame, sizeof(h265_frame), true, 0xBB);

    int initial_count = atomic_load(&ldc.frame_count);
    ASSERT_TRUE(nano.track_mids[0] >= 0);
    rc = interop_nanortc_media_send_video(&nano, (uint8_t)nano.track_mids[0], nano_get_millis(),
                                          h265_frame, sizeof(h265_frame));
    ASSERT_OK(rc);

    rc = wait_frame_count(&ldc.frame_count, initial_count + 1, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    interop_media_frame_t received;
    rc = interop_libdatachannel_media_get_last_frame(&ldc, &received);
    ASSERT_OK(rc);
    ASSERT_TRUE(received.len > 0);

    teardown_video_pair(&pipe, &nano, &ldc);
}

/* ----------------------------------------------------------------
 * Test: H.265 mid-stream parameter-set refresh + IDR (Phase 14)
 *
 * An adaptive geometry change re-emits the in-band VPS/SPS/PPS followed by an
 * IDR as a single multi-NAL access unit. Verify a real receiver (libdatachannel)
 * consumes that spec-switch wire format mid-session, with no renegotiation: send
 * an initial IDR, then the multi-NAL [VPS][SPS][PPS][IDR] AU, and confirm both
 * are received.
 * ---------------------------------------------------------------- */

TEST(test_interop_video_h265_midstream_refresh)
{
    interop_sig_pipe_t pipe;
    interop_nanortc_media_peer_t nano;
    interop_libdatachannel_media_peer_t ldc;

    int rc = setup_video_pair_codec(&pipe, &nano, &ldc, NANORTC_DIR_SENDONLY, LDC_DIR_RECVONLY,
                                    NANORTC_CODEC_H265, LDC_CODEC_H265, 98, &kSampleH265Sets);
    ASSERT_OK(rc);
    rc = interop_libdatachannel_media_wait_flag(&ldc.connected, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_libdatachannel_media_wait_flag((atomic_int *)&nano.connected, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);
    interop_sleep_ms(200);
    ASSERT_TRUE(nano.track_mids[0] >= 0);

    /* Initial IDR (single NAL). */
    uint8_t idr1[64];
    build_h265_frame(idr1, sizeof(idr1), true, 0xC1);
    int initial = atomic_load(&ldc.frame_count);
    rc = interop_nanortc_media_send_video(&nano, (uint8_t)nano.track_mids[0], nano_get_millis(),
                                          idr1, sizeof(idr1));
    ASSERT_OK(rc);
    rc = wait_frame_count(&ldc.frame_count, initial + 1, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    /* The spec-switch access unit: re-emitted in-band VPS/SPS/PPS + IDR. */
    static const uint8_t refresh_au[] = {
        0, 0, 0, 1, 0x40, 0x01, 0x0C, 0x02, 0xEE,                         /* VPS */
        0, 0, 0, 1, 0x42, 0x01, 0x02, 0x02, 0x55,                         /* SPS */
        0, 0, 0, 1, 0x44, 0x01, 0xC2, 0x33,                               /* PPS */
        0, 0, 0, 1, 0x26, 0x01, 0xD2, 0xD2, 0xD2, 0xD2, 0xD2, 0xD2, 0xD2, /* IDR */
    };
    initial = atomic_load(&ldc.frame_count);
    rc = interop_nanortc_media_send_video(&nano, (uint8_t)nano.track_mids[0], nano_get_millis(),
                                          refresh_au, sizeof(refresh_au));
    ASSERT_OK(rc);
    rc = wait_frame_count(&ldc.frame_count, initial + 1, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    interop_media_frame_t received;
    rc = interop_libdatachannel_media_get_last_frame(&ldc, &received);
    ASSERT_OK(rc);
    ASSERT_TRUE(received.len > 0);

    teardown_video_pair(&pipe, &nano, &ldc);
}

/* ----------------------------------------------------------------
 * Test: H.265 FU fragmentation (RFC 7798 §4.4.3)
 *
 * 1800-byte IDR > 1200-byte MTU forces FU fragmentation on the sender
 * and reassembly on the receiver.
 * ---------------------------------------------------------------- */

TEST(test_interop_video_h265_fu_fragmentation)
{
    interop_sig_pipe_t pipe;
    interop_nanortc_media_peer_t nano;
    interop_libdatachannel_media_peer_t ldc;

    int rc = setup_video_pair_codec(&pipe, &nano, &ldc, NANORTC_DIR_RECVONLY, LDC_DIR_SENDONLY,
                                    NANORTC_CODEC_H265, LDC_CODEC_H265, 98, NULL);
    ASSERT_OK(rc);

    rc = interop_libdatachannel_media_wait_flag(&ldc.connected, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_libdatachannel_media_wait_flag(&ldc.track_open, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);
    rc = interop_libdatachannel_media_wait_flag((atomic_int *)&nano.connected, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    uint8_t large_frame[1800];
    build_h265_frame(large_frame, sizeof(large_frame), true, 0xEE);

    int initial_count = atomic_load(&nano.frame_count);
    rc = interop_libdatachannel_media_send(&ldc, 0, large_frame, sizeof(large_frame));
    ASSERT_TRUE(rc >= 0);

    rc = wait_frame_count(&nano.frame_count, initial_count + 1, INTEROP_TIMEOUT_MS);
    ASSERT_OK(rc);

    interop_media_frame_t received;
    rc = interop_nanortc_media_get_last_frame(&nano, &received);
    ASSERT_OK(rc);
    ASSERT_TRUE(received.len > 0);
    ASSERT_TRUE(received.is_keyframe);

    teardown_video_pair(&pipe, &nano, &ldc);
}

#endif /* NANORTC_FEATURE_H265 */

/* ----------------------------------------------------------------
 * Test runner
 * ---------------------------------------------------------------- */

TEST_MAIN_BEGIN("interop-video")
RUN(test_interop_video_handshake);
RUN(test_interop_video_h264_ldc_to_nano);
RUN(test_interop_video_h264_nano_to_ldc);
RUN(test_interop_video_keyframe);
RUN(test_interop_audio_video_combined);
#if NANORTC_FEATURE_H265
RUN(test_interop_video_h265_ldc_to_nano);
RUN(test_interop_video_h265_nano_to_ldc);
RUN(test_interop_video_h265_midstream_refresh);
RUN(test_interop_video_h265_fu_fragmentation);
#endif
TEST_MAIN_END
