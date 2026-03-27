/*
 * nanortc example — Linux media sender
 *
 * Sends H.264 video and/or Opus audio from sample files to a browser.
 * DataChannel is also available for bidirectional messaging.
 *
 * Usage:
 *   ./linux_media_send [-p port] [-v video_dir] [-a audio_dir]
 *
 * Sample data:
 *   -v path/to/h264SampleFrames   (frame-NNNN.h264, 25fps)
 *   -a path/to/opusSampleFrames   (sample-NNN.opus, 20ms)
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nano_rtc_internal.h"
#include "nano_crypto.h"
#include "run_loop.h"
#include "signaling.h"
#include "media_source.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

static nano_run_loop_t loop;
static int connected = 0;

static void on_signal(int sig)
{
    (void)sig;
    nano_run_loop_stop(&loop);
}

static void on_event(nano_rtc_t *rtc, const nano_event_t *evt, void *userdata)
{
    (void)rtc;
    (void)userdata;

    switch (evt->type) {
    case NANO_EVENT_SCTP_CONNECTED:
        fprintf(stderr, "[event] Connected — starting media\n");
        connected = 1;
        break;

    case NANO_EVENT_DATACHANNEL_STRING:
        fprintf(stderr, "[event] DC: %.*s\n", (int)evt->len, (char *)evt->data);
        break;

#if NANO_FEATURE_AUDIO
    case NANO_EVENT_AUDIO_DATA:
        /* Received audio from remote — could play it */
        break;
#endif

#if NANO_FEATURE_VIDEO
    case NANO_EVENT_KEYFRAME_REQUEST:
        fprintf(stderr, "[event] Keyframe requested\n");
        break;
#endif

    case NANO_EVENT_DISCONNECTED:
        fprintf(stderr, "[event] Disconnected\n");
        connected = 0;
        nano_run_loop_stop(&loop);
        break;

    default:
        break;
    }
}

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [-p port] [-v video_dir] [-a audio_dir]\n", prog);
    fprintf(stderr, "  -p port       UDP port (default: 9999)\n");
    fprintf(stderr, "  -v video_dir  H.264 sample frames directory\n");
    fprintf(stderr, "  -a audio_dir  Opus sample frames directory\n");
    fprintf(stderr, "\nSample data (git submodule at examples/sample_data):\n");
    fprintf(stderr, "  -v examples/sample_data/h264SampleFrames\n");
    fprintf(stderr, "  -a examples/sample_data/opusSampleFrames\n");
}

int main(int argc, char *argv[])
{
    uint16_t port = 9999;
    const char *video_dir = NULL;
    const char *audio_dir = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-v") == 0 && i + 1 < argc) {
            video_dir = argv[++i];
        } else if (strcmp(argv[i], "-a") == 0 && i + 1 < argc) {
            audio_dir = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* 1. Init media sources */
    nano_media_source_t video_src, audio_src;
    int has_video = 0, has_audio = 0;

#if NANO_FEATURE_VIDEO
    if (video_dir) {
        if (nano_media_source_init(&video_src, NANO_MEDIA_H264, video_dir) == 0) {
            has_video = 1;
            fprintf(stderr, "Video source: %s (H.264, 25fps)\n", video_dir);
        } else {
            fprintf(stderr, "Warning: cannot open video samples in %s\n", video_dir);
        }
    }
#endif

#if NANO_FEATURE_AUDIO
    if (audio_dir) {
        if (nano_media_source_init(&audio_src, NANO_MEDIA_OPUS, audio_dir) == 0) {
            has_audio = 1;
            fprintf(stderr, "Audio source: %s (Opus, 20ms)\n", audio_dir);
        } else {
            fprintf(stderr, "Warning: cannot open audio samples in %s\n", audio_dir);
        }
    }
#endif

    if (!has_video && !has_audio) {
        fprintf(stderr, "No media sources specified. Use -v and/or -a.\n");
        fprintf(stderr, "Running in DataChannel-only mode.\n");
    }

    /* 2. Init nanortc */
    nano_rtc_t rtc;
    nano_rtc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

#if defined(NANORTC_CRYPTO_OPENSSL)
    cfg.crypto = nano_crypto_openssl();
#else
    cfg.crypto = nano_crypto_mbedtls();
#endif
    cfg.role = NANO_ROLE_CONTROLLED;

#if NANO_FEATURE_AUDIO
    if (has_audio) {
        cfg.audio_codec = NANO_CODEC_OPUS;
        cfg.audio_sample_rate = 48000;
        cfg.audio_channels = 2;
        cfg.audio_direction = NANO_DIR_SENDONLY;
        cfg.jitter_depth_ms = 100;
    }
#endif

#if NANO_FEATURE_VIDEO
    if (has_video) {
        cfg.video_codec = NANO_CODEC_H264;
        cfg.video_direction = NANO_DIR_SENDONLY;
    }
#endif

    int rc = nano_rtc_init(&rtc, &cfg);
    if (rc != NANO_OK) {
        fprintf(stderr, "nano_rtc_init failed: %d\n", rc);
        return 1;
    }

    /* 3. Event loop */
    rc = nano_run_loop_init(&loop, &rtc, NULL, port);
    if (rc < 0) {
        fprintf(stderr, "Failed to bind UDP port %d\n", port);
        return 1;
    }
    nano_run_loop_set_event_cb(&loop, on_event, NULL);

    fprintf(stderr, "nanortc media sender (port=%d, DC=%d AUDIO=%d VIDEO=%d)\n", port,
            NANO_FEATURE_DATACHANNEL, NANO_FEATURE_AUDIO, NANO_FEATURE_VIDEO);

    /* 4. Signaling */
    nano_signaling_t sig;
    nano_signaling_init(&sig, NANO_SIG_STDIN);

    char offer[4096];
    rc = nano_signaling_recv_offer(&sig, offer, sizeof(offer));
    if (rc < 0) {
        fprintf(stderr, "Failed to read SDP offer\n");
        return 1;
    }

    char answer[4096];
    rc = nano_accept_offer(&rtc, offer, answer, sizeof(answer), NULL);
    if (rc != NANO_OK) {
        fprintf(stderr, "nano_accept_offer failed: %d (%s)\n", rc, nano_err_to_name(rc));
        return 1;
    }
    nano_signaling_send_answer(&sig, answer);
    nano_signaling_destroy(&sig);

    /* 5. Main loop: event loop + media pacing */
    fprintf(stderr, "Entering event loop... (Ctrl+C to quit)\n");

    uint8_t frame_buf[NANO_MEDIA_MAX_FRAME_SIZE];
    uint32_t next_video_ms = 0;
    uint32_t next_audio_ms = 0;

    loop.running = 1;
    while (loop.running) {
        nano_run_loop_step(&loop);

        if (!connected) {
            continue;
        }

        uint32_t now = nano_get_millis();

#if NANO_FEATURE_VIDEO
        /* Send video frame at 25fps */
        if (has_video && now >= next_video_ms) {
            size_t frame_len;
            uint32_t ts;
            if (nano_media_source_next_frame(&video_src, frame_buf, sizeof(frame_buf),
                                             &frame_len, &ts) == 0) {
                int is_kf = (video_src.frame_index == 2); /* frame after reset = keyframe */
                nano_send_video(&rtc, ts, frame_buf, frame_len, is_kf);
            }
            next_video_ms = now + nano_media_source_interval_ms(&video_src);
        }
#endif

#if NANO_FEATURE_AUDIO
        /* Send audio frame at 50fps (20ms) */
        if (has_audio && now >= next_audio_ms) {
            size_t frame_len;
            uint32_t ts;
            if (nano_media_source_next_frame(&audio_src, frame_buf, sizeof(frame_buf),
                                             &frame_len, &ts) == 0) {
                nano_send_audio(&rtc, ts, frame_buf, frame_len);
            }
            next_audio_ms = now + nano_media_source_interval_ms(&audio_src);
        }
#endif
    }

    /* 6. Cleanup */
    nano_run_loop_destroy(&loop);
    nano_rtc_destroy(&rtc);
    if (has_video) nano_media_source_destroy(&video_src);
    if (has_audio) nano_media_source_destroy(&audio_src);

    fprintf(stderr, "Done.\n");
    return 0;
}
