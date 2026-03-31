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
#include "nanortc_crypto.h"
#include "run_loop.h"
#include "signaling.h"
#include "media_source.h"
#include "h264_utils.h"

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

static void on_event(nanortc_t *rtc, const nanortc_event_t *evt, void *userdata)
{
    (void)rtc;
    (void)userdata;

    switch (evt->type) {
    case NANORTC_EV_CONNECTED:
        fprintf(stderr, "[event] Connected — starting media\n");
        connected = 1;
        break;

    case NANORTC_EV_CHANNEL_DATA:
        if (!evt->channel_data.binary) {
            fprintf(stderr, "[event] DC: %.*s\n", (int)evt->channel_data.len,
                    (char *)evt->channel_data.data);
        }
        break;

#if NANORTC_FEATURE_AUDIO || NANORTC_FEATURE_VIDEO
    case NANORTC_EV_MEDIA_DATA:
        /* Received media from remote — could play/render it */
        break;
#endif

#if NANORTC_FEATURE_VIDEO
    case NANORTC_EV_KEYFRAME_REQUEST:
        fprintf(stderr, "[event] Keyframe requested\n");
        break;
#endif

    case NANORTC_EV_DISCONNECTED:
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

#if NANORTC_FEATURE_VIDEO
    if (video_dir) {
        if (nano_media_source_init(&video_src, NANORTC_MEDIA_H264, video_dir) == 0) {
            has_video = 1;
            fprintf(stderr, "Video source: %s (H.264, 25fps)\n", video_dir);
        } else {
            fprintf(stderr, "Warning: cannot open video samples in %s\n", video_dir);
        }
    }
#endif

#if NANORTC_FEATURE_AUDIO
    if (audio_dir) {
        if (nano_media_source_init(&audio_src, NANORTC_MEDIA_OPUS, audio_dir) == 0) {
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
    nanortc_t rtc;
    nanortc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

#if defined(NANORTC_CRYPTO_OPENSSL)
    cfg.crypto = nanortc_crypto_openssl();
#else
    cfg.crypto = nanortc_crypto_mbedtls();
#endif
    cfg.role = NANORTC_ROLE_CONTROLLED;

#if NANORTC_FEATURE_AUDIO
    if (has_audio) {
        cfg.audio_codec = NANORTC_CODEC_OPUS;
        cfg.audio_sample_rate = 48000;
        cfg.audio_channels = 2;
        cfg.audio_direction = NANORTC_DIR_SENDONLY;
        cfg.jitter_depth_ms = 100;
    }
#endif

#if NANORTC_FEATURE_VIDEO
    if (has_video) {
        cfg.video_codec = NANORTC_CODEC_H264;
        cfg.video_direction = NANORTC_DIR_SENDONLY;
    }
#endif

    int rc = nanortc_init(&rtc, &cfg);
    if (rc != NANORTC_OK) {
        fprintf(stderr, "nanortc_init failed: %d\n", rc);
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
            NANORTC_FEATURE_DATACHANNEL, NANORTC_FEATURE_AUDIO, NANORTC_FEATURE_VIDEO);

    /* 4. Signaling */
    nano_signaling_t sig;
    nano_signaling_init(&sig, NANORTC_SIG_STDIN);

    char offer[4096];
    rc = nano_signaling_recv_offer(&sig, offer, sizeof(offer));
    if (rc < 0) {
        fprintf(stderr, "Failed to read SDP offer\n");
        return 1;
    }

    char answer[4096];
    rc = nanortc_accept_offer(&rtc, offer, answer, sizeof(answer), NULL);
    if (rc != NANORTC_OK) {
        fprintf(stderr, "nanortc_accept_offer failed: %d (%s)\n", rc, nanortc_err_name(rc));
        return 1;
    }
    nano_signaling_send_answer(&sig, answer);
    nano_signaling_destroy(&sig);

    /* 5. Main loop: event loop + media pacing */
    fprintf(stderr, "Entering event loop... (Ctrl+C to quit)\n");

    uint8_t frame_buf[NANORTC_MEDIA_MAX_FRAME_SIZE];
    uint32_t video_epoch_ms = 0;
    uint32_t video_frame_count = 0;
    uint32_t audio_epoch_ms = 0;
    uint32_t audio_frame_count = 0;

    if (has_video || has_audio) {
        loop.max_poll_ms = 5; /* 5ms poll for smooth media pacing */
    }

    loop.running = 1;
    while (loop.running) {
        nano_run_loop_step(&loop);

        if (!connected) {
            continue;
        }

        uint32_t now = nano_get_millis();

#if NANORTC_FEATURE_VIDEO
        /* Send video frames at 25fps (epoch-based for drift-free timing) */
        if (has_video) {
            if (video_epoch_ms == 0)
                video_epoch_ms = now;
            uint32_t target_frames = (now - video_epoch_ms) / 40;
            if (target_frames - video_frame_count > 2) {
                video_frame_count = target_frames - 1;
            }
            if (video_frame_count < target_frames) {
                size_t frame_len = 0;
                uint32_t ts_ms = 0;
                if (nano_media_source_next_frame(&video_src, frame_buf, sizeof(frame_buf),
                                                 &frame_len, &ts_ms) == 0) {
                    /* RTP timestamp: 90kHz clock, 3600 ticks per frame at 25fps */
                    uint32_t video_ts_rtp = video_frame_count * 3600;

                    /* Split Annex-B into individual NALUs */
                    size_t offset = 0;
                    size_t nal_len = 0;
                    const uint8_t *nal;
                    while ((nal = annex_b_find_nal(frame_buf, frame_len, &offset, &nal_len)) !=
                           NULL) {
                        int flags = 0;
                        if ((nal[0] & 0x1F) == 5)
                            flags |= NANORTC_VIDEO_FLAG_KEYFRAME;
                        size_t peek_off = offset;
                        size_t peek_len = 0;
                        if (annex_b_find_nal(frame_buf, frame_len, &peek_off, &peek_len) == NULL) {
                            flags |= NANORTC_VIDEO_FLAG_MARKER;
                        }
                        nanortc_send_video(&rtc, video_ts_rtp, nal, nal_len, flags);
                    }
                    video_frame_count++;
                }
            }
        }
#endif

#if NANORTC_FEATURE_AUDIO
        /* Send audio frames at 20ms intervals (epoch-based for drift-free timing) */
        if (has_audio) {
            if (audio_epoch_ms == 0)
                audio_epoch_ms = now;
            uint32_t target_frames = (now - audio_epoch_ms) / 20;
            if (target_frames - audio_frame_count > 2) {
                audio_frame_count = target_frames - 1;
            }
            if (audio_frame_count < target_frames) {
                size_t frame_len = 0;
                uint32_t ts_ms = 0;
                if (nano_media_source_next_frame(&audio_src, frame_buf, sizeof(frame_buf),
                                                 &frame_len, &ts_ms) == 0) {
                    uint32_t audio_ts_rtp = audio_frame_count * 960;
                    nanortc_send_audio(&rtc, audio_ts_rtp, frame_buf, frame_len);
                    audio_frame_count++;
                }
            }
        }
#endif
    }

    /* 6. Cleanup */
    nano_run_loop_destroy(&loop);
    nanortc_destroy(&rtc);
    if (has_video)
        nano_media_source_destroy(&video_src);
    if (has_audio)
        nano_media_source_destroy(&audio_src);

    fprintf(stderr, "Done.\n");
    return 0;
}
