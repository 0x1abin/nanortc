/*
 * macos_camera — Real-time macOS camera/mic → browser via nanortc
 *
 * Captures camera video (AVFoundation) and microphone audio, encodes
 * to H.264 (VideoToolbox) and Opus (libopus), and streams to multiple
 * browser viewers using nanortc WebRTC.
 *
 * Supports multiple simultaneous viewers and reconnection.
 *
 * Usage:
 *   ./macos_camera [-p udp_port] [-s host:port]
 *
 * Default: signaling at localhost:8765, base UDP port 9999.
 *
 * SPDX-License-Identifier: MIT
 */

#import <Foundation/Foundation.h>

#include "nanortc.h"
#include "nanortc_crypto.h"
#include "http_signaling.h"
#include "multi_session.h"
#include "av_capture.h"
#include "vt_encoder.h"

#include <opus.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/select.h>

/* ----------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------- */

#define VIDEO_WIDTH        1280
#define VIDEO_HEIGHT       720
#define VIDEO_FPS          30
#define VIDEO_BITRATE_KBPS 3000
#define VIDEO_KEYFRAME_S   2

#define OPUS_SAMPLE_RATE   48000
#define OPUS_CHANNELS      1
#define OPUS_FRAME_MS      20
#define OPUS_FRAME_SIZE    (OPUS_SAMPLE_RATE * OPUS_FRAME_MS / 1000) /* 960 samples */
#define OPUS_MAX_PACKET    4000
#define OPUS_BITRATE       64000

#define FRAME_QUEUE_SIZE   16
#define MAX_SESSIONS       4
#define MAX_LOCAL_IPS      8

/* ----------------------------------------------------------------
 * Thread-safe frame queue (pipe + mutex)
 * ---------------------------------------------------------------- */

typedef struct {
    uint8_t type; /* 0 = audio, 1 = video */
    uint8_t *data;
    size_t len;
    uint32_t pts_ms;
} queued_frame_t;

typedef struct {
    queued_frame_t frames[FRAME_QUEUE_SIZE];
    int head;
    int tail;
    int count;
    pthread_mutex_t lock;
    int wake_pipe[2]; /* write end wakes select() */
} frame_queue_t;

static void fq_init(frame_queue_t *q)
{
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->lock, NULL);
    pipe(q->wake_pipe);
}

static void fq_destroy(frame_queue_t *q)
{
    pthread_mutex_lock(&q->lock);
    for (int i = 0; i < q->count; i++) {
        int idx = (q->head + i) % FRAME_QUEUE_SIZE;
        free(q->frames[idx].data);
    }
    pthread_mutex_unlock(&q->lock);
    pthread_mutex_destroy(&q->lock);
    close(q->wake_pipe[0]);
    close(q->wake_pipe[1]);
}

static void fq_push(frame_queue_t *q, uint8_t type, const uint8_t *data, size_t len,
                     uint32_t pts_ms)
{
    pthread_mutex_lock(&q->lock);

    /* If full, drop oldest frame */
    if (q->count == FRAME_QUEUE_SIZE) {
        free(q->frames[q->head].data);
        q->head = (q->head + 1) % FRAME_QUEUE_SIZE;
        q->count--;
    }

    int idx = (q->head + q->count) % FRAME_QUEUE_SIZE;
    q->frames[idx].type = type;
    q->frames[idx].data = malloc(len);
    if (q->frames[idx].data) {
        memcpy(q->frames[idx].data, data, len);
        q->frames[idx].len = len;
        q->frames[idx].pts_ms = pts_ms;
        q->count++;
    }

    pthread_mutex_unlock(&q->lock);

    /* Wake the event loop */
    char c = 'F';
    write(q->wake_pipe[1], &c, 1);
}

static int fq_pop(frame_queue_t *q, queued_frame_t *out)
{
    pthread_mutex_lock(&q->lock);
    if (q->count == 0) {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    *out = q->frames[q->head];
    q->frames[q->head].data = NULL; /* ownership transferred */
    q->head = (q->head + 1) % FRAME_QUEUE_SIZE;
    q->count--;
    pthread_mutex_unlock(&q->lock);
    return 0;
}

/* ----------------------------------------------------------------
 * Session pool (one session per connected viewer)
 * ---------------------------------------------------------------- */

static nano_session_t g_sessions[MAX_SESSIONS];
static nano_local_ip_t g_local_ips[MAX_LOCAL_IPS];
static nano_session_pool_t g_pool;

/* ----------------------------------------------------------------
 * Application context (shared across all sessions)
 * ---------------------------------------------------------------- */

typedef struct {
    frame_queue_t fq;
    OpusEncoder *opus_enc;
    int16_t pcm_accum[OPUS_FRAME_SIZE]; /* accumulator for 20ms frames */
    int pcm_accum_pos;
    uint32_t audio_pts_ms;
} app_ctx_t;

static volatile sig_atomic_t g_quit;

static void on_signal(int sig)
{
    (void)sig;
    g_quit = 1;
}

/* ----------------------------------------------------------------
 * Track setup + event callback (injected into the session pool)
 * ---------------------------------------------------------------- */

static int macos_track_setup(nano_session_t *s, void *userdata)
{
    (void)userdata;
#if NANORTC_FEATURE_AUDIO
    s->audio_mid = nanortc_add_audio_track(&s->rtc, NANORTC_DIR_SENDONLY,
                                           NANORTC_CODEC_OPUS, 48000, 2);
#endif
#if NANORTC_FEATURE_VIDEO
    s->video_mid = nanortc_add_video_track(&s->rtc, NANORTC_DIR_SENDONLY,
                                           NANORTC_CODEC_H264);
#endif
    return 0;
}

static void macos_on_event(nano_session_t *s, const nanortc_event_t *evt, void *userdata)
{
    (void)userdata;
    switch (evt->type) {
    case NANORTC_EV_ICE_STATE_CHANGE:
        if (evt->ice_state == NANORTC_ICE_STATE_CONNECTED) {
            fprintf(stderr, "[session %d] ICE connected\n", s->viewer_id);
        }
        break;
    case NANORTC_EV_CONNECTED:
        fprintf(stderr, "[session %d] Media connected — forcing keyframe\n", s->viewer_id);
        vt_encoder_force_keyframe();
        break;
    case NANORTC_EV_KEYFRAME_REQUEST:
        fprintf(stderr, "[session %d] Keyframe requested (PLI)\n", s->viewer_id);
        vt_encoder_force_keyframe();
        break;
    case NANORTC_EV_DISCONNECTED:
        fprintf(stderr, "[session %d] Disconnected\n", s->viewer_id);
        break;
    default:
        break;
    }
}

/* ----------------------------------------------------------------
 * AVFoundation callbacks (called on dispatch queues)
 * ---------------------------------------------------------------- */

static void on_video_frame(void *ctx, CVPixelBufferRef pixbuf, CMTime pts)
{
    (void)ctx;
    /* Always encode — sessions may connect at any time */
    vt_encoder_encode(pixbuf, pts);
}

static void on_encoded_video(void *ctx, const uint8_t *annex_b, size_t len, uint32_t pts_ms,
                             bool is_keyframe)
{
    app_ctx_t *app = (app_ctx_t *)ctx;
    (void)is_keyframe;
    fq_push(&app->fq, 1, annex_b, len, pts_ms);
}

static void on_audio_samples(void *ctx, const int16_t *pcm, size_t sample_count, CMTime pts)
{
    app_ctx_t *app = (app_ctx_t *)ctx;
    if (!app->opus_enc)
        return;

    /* Accumulate PCM samples into 960-sample (20ms) frames and encode */
    size_t offset = 0;
    while (offset < sample_count) {
        size_t need = (size_t)(OPUS_FRAME_SIZE - app->pcm_accum_pos);
        size_t avail = sample_count - offset;
        size_t copy = avail < need ? avail : need;

        memcpy(&app->pcm_accum[app->pcm_accum_pos], &pcm[offset],
               copy * sizeof(int16_t));
        app->pcm_accum_pos += (int)copy;
        offset += copy;

        if (app->pcm_accum_pos == OPUS_FRAME_SIZE) {
            uint8_t opus_buf[OPUS_MAX_PACKET];
            int nbytes = opus_encode(app->opus_enc, app->pcm_accum, OPUS_FRAME_SIZE,
                                     opus_buf, OPUS_MAX_PACKET);
            if (nbytes > 0) {
                fq_push(&app->fq, 0, opus_buf, (size_t)nbytes, app->audio_pts_ms);
                app->audio_pts_ms += OPUS_FRAME_MS;
            }
            app->pcm_accum_pos = 0;
        }
    }
}

/* ----------------------------------------------------------------
 * Signaling poll: accept new offers and ICE candidates
 * ---------------------------------------------------------------- */

static uint32_t last_poll_ms;

uint32_t nano_get_millis(void); /* from run_loop_linux.c */

static void poll_signaling(http_sig_t *sig, const nanortc_config_t *cfg)
{
    uint32_t now = nano_get_millis();
    if (now - last_poll_ms < 100)
        return; /* throttle polling */
    last_poll_ms = now;

    char type[32];
    char payload[HTTP_SIG_BUF_SIZE];
    int from = -1;

    int rc = http_sig_recv_from(sig, type, sizeof(type), payload, sizeof(payload),
                                &from, 0);
    if (rc != 0)
        return; /* timeout or error */

    if (from < 0)
        return; /* no sender ID */

    if (strcmp(type, "offer") == 0) {
        fprintf(stderr, "[sig] Got offer from viewer %d (%zu bytes)\n",
                from, strlen(payload));
        nano_session_create(&g_pool, from, payload, cfg, sig);

    } else if (strcmp(type, "candidate") == 0) {
        nano_session_t *s = nano_session_find_by_viewer(&g_pool, from);
        if (s && payload[0] != '\0') {
            fprintf(stderr, "[sig] ICE candidate from viewer %d\n", from);
            nanortc_add_remote_candidate(&s->rtc, payload);
        }
    }
}

/* ----------------------------------------------------------------
 * Multi-session event loop
 * ---------------------------------------------------------------- */

static void run_event_loop(app_ctx_t *ctx, http_sig_t *sig,
                           const nanortc_config_t *cfg)
{
    int pipe_fd = ctx->fq.wake_pipe[0];

    while (!g_quit) {
        uint32_t timeout_ms = 5; /* 5ms for smooth media pacing */

        /* Drain output queues for all sessions */
        nano_session_pool_dispatch_all(&g_pool, &timeout_ms);

        /* Build fd_set: all session UDP fds + wake pipe */
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(pipe_fd, &rset);
        int maxfd = pipe_fd;
        nano_session_pool_add_fds(&g_pool, &rset, &maxfd);

        struct timeval tv = {
            .tv_sec = timeout_ms / 1000,
            .tv_usec = (timeout_ms % 1000) * 1000,
        };

        int ret = select(maxfd + 1, &rset, NULL, NULL, &tv);
        uint32_t now = nano_get_millis();

        /* Pump UDP input + internal timers for all active sessions */
        nano_session_pool_handle_udp(&g_pool, ret, &rset, now);

        /* Handle wake pipe: broadcast encoded frames to all connected sessions */
        if (ret > 0 && FD_ISSET(pipe_fd, &rset)) {
            char drain[64];
            read(pipe_fd, drain, sizeof(drain));

            queued_frame_t frame;
            while (fq_pop(&ctx->fq, &frame) == 0) {
                for (int i = 0; i < MAX_SESSIONS; i++) {
                    nano_session_t *s = &g_sessions[i];
                    if (!s->active || !s->media_connected)
                        continue;

                    if (frame.type == 0 && s->audio_mid >= 0) {
                        nanortc_send_audio(&s->rtc, (uint8_t)s->audio_mid,
                                           frame.pts_ms, frame.data, frame.len);
                    } else if (frame.type == 1 && s->video_mid >= 0) {
                        nanortc_send_video(&s->rtc, (uint8_t)s->video_mid,
                                           frame.pts_ms, frame.data, frame.len);
                    }
                }
                free(frame.data);
            }
        }

        /* Dispatch any new outputs generated by handle_input / send_* */
        nano_session_pool_dispatch_all(&g_pool, &timeout_ms);

        /* Cleanup sessions whose active flag was cleared on DISCONNECTED */
        nano_session_pool_cleanup(&g_pool);

        /* Poll signaling for new offers / ICE candidates */
        poll_signaling(sig, cfg);
    }
}

/* ----------------------------------------------------------------
 * Usage
 * ---------------------------------------------------------------- */

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [options]\n", prog);
    fprintf(stderr, "  -b IP          Bind/candidate IP (default: auto-detect all interfaces)\n");
    fprintf(stderr, "  -s HOST:PORT   Signaling server (default: localhost:8765)\n");
}

/* ----------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    @autoreleasepool {
        char bind_ip[64] = "";
        char sig_host[256] = "localhost";
        uint16_t sig_port = 8765;

        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
                size_t len = strlen(argv[++i]);
                if (len >= sizeof(bind_ip))
                    len = sizeof(bind_ip) - 1;
                memcpy(bind_ip, argv[i], len);
                bind_ip[len] = '\0';
            } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
                i++;
                char *colon = strrchr(argv[i], ':');
                if (colon) {
                    size_t hlen = (size_t)(colon - argv[i]);
                    if (hlen >= sizeof(sig_host))
                        hlen = sizeof(sig_host) - 1;
                    memcpy(sig_host, argv[i], hlen);
                    sig_host[hlen] = '\0';
                    sig_port = (uint16_t)atoi(colon + 1);
                } else {
                    size_t hlen = strlen(argv[i]);
                    if (hlen >= sizeof(sig_host))
                        hlen = sizeof(sig_host) - 1;
                    memcpy(sig_host, argv[i], hlen);
                    sig_host[hlen] = '\0';
                }
            } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
                usage(argv[0]);
                return 0;
            }
        }

        signal(SIGINT, on_signal);
        signal(SIGTERM, on_signal);

        /* 1. Init shared resources */
        app_ctx_t ctx;
        memset(&ctx, 0, sizeof(ctx));
        fq_init(&ctx.fq);

        /* Opus encoder */
        int opus_err;
        ctx.opus_enc = opus_encoder_create(OPUS_SAMPLE_RATE, OPUS_CHANNELS,
                                           OPUS_APPLICATION_VOIP, &opus_err);
        if (opus_err != OPUS_OK) {
            fprintf(stderr, "opus_encoder_create: %s\n", opus_strerror(opus_err));
            return 1;
        }
        opus_encoder_ctl(ctx.opus_enc, OPUS_SET_BITRATE(OPUS_BITRATE));
        opus_encoder_ctl(ctx.opus_enc, OPUS_SET_INBAND_FEC(1));
        fprintf(stderr, "[opus] Encoder ready (48kHz mono, %d bps)\n", OPUS_BITRATE);

        /* VideoToolbox H.264 encoder */
        vt_encoder_config_t vt_cfg = {
            .width = VIDEO_WIDTH,
            .height = VIDEO_HEIGHT,
            .fps = VIDEO_FPS,
            .bitrate_kbps = VIDEO_BITRATE_KBPS,
            .keyframe_interval_s = VIDEO_KEYFRAME_S,
            .callback = on_encoded_video,
            .userdata = &ctx,
        };
        if (vt_encoder_init(&vt_cfg) != 0) {
            fprintf(stderr, "VideoToolbox encoder init failed\n");
            opus_encoder_destroy(ctx.opus_enc);
            return 1;
        }

        /* 2. Set up session pool + local IP candidates */
        nano_session_pool_init(&g_pool, g_sessions, MAX_SESSIONS);
        g_pool.track_setup = macos_track_setup;
        g_pool.on_event = macos_on_event;

        int local_ip_count;
        if (bind_ip[0] != '\0') {
            /* User-supplied single IP */
            size_t len = strlen(bind_ip);
            if (len >= sizeof(g_local_ips[0].ip))
                len = sizeof(g_local_ips[0].ip) - 1;
            memcpy(g_local_ips[0].ip, bind_ip, len);
            g_local_ips[0].ip[len] = '\0';
            local_ip_count = 1;
        } else {
            local_ip_count = nano_enumerate_local_ipv4(g_local_ips, MAX_LOCAL_IPS);
            /* Capture the first IP for the startup log. */
            size_t len = strlen(g_local_ips[0].ip);
            if (len >= sizeof(bind_ip)) len = sizeof(bind_ip) - 1;
            memcpy(bind_ip, g_local_ips[0].ip, len);
            bind_ip[len] = '\0';
        }
        g_pool.local_ips = g_local_ips;
        g_pool.local_ip_count = local_ip_count;

        /* 3. Join signaling server as host */
        http_sig_t sig;
        int rc = http_sig_join_as_host(&sig, sig_host, sig_port);
        if (rc < 0) {
            fprintf(stderr, "Failed to join signaling server %s:%u as host\n",
                    sig_host, sig_port);
            vt_encoder_destroy();
            opus_encoder_destroy(ctx.opus_enc);
            return 1;
        }

        /* 4. Prepare nanortc config (shared, used per-session) */
        nanortc_config_t rtc_cfg;
        memset(&rtc_cfg, 0, sizeof(rtc_cfg));
#if defined(NANORTC_CRYPTO_OPENSSL)
        rtc_cfg.crypto = nanortc_crypto_openssl();
#else
        rtc_cfg.crypto = nanortc_crypto_mbedtls();
#endif
        rtc_cfg.role = NANORTC_ROLE_CONTROLLED; /* always answerer */

        /* 5. Start AVFoundation capture */
        av_capture_config_t av_cfg = {
            .video_width = VIDEO_WIDTH,
            .video_height = VIDEO_HEIGHT,
            .video_fps = VIDEO_FPS,
            .audio_sample_rate = OPUS_SAMPLE_RATE,
            .audio_channels = OPUS_CHANNELS,
            .video_cb = on_video_frame,
            .audio_cb = on_audio_samples,
            .userdata = &ctx,
        };
        if (av_capture_start(&av_cfg) != 0) {
            fprintf(stderr, "AVFoundation capture start failed\n");
            http_sig_leave(&sig);
            vt_encoder_destroy();
            opus_encoder_destroy(ctx.opus_enc);
            return 1;
        }

        fprintf(stderr, "macos_camera (host mode, ip=%s, sig=%s:%u, max_viewers=%d)\n",
                bind_ip, sig_host, sig_port, MAX_SESSIONS);
        fprintf(stderr, "Waiting for viewers (Ctrl+C to stop)...\n");

        /* 6. Run multi-session event loop */
        run_event_loop(&ctx, &sig, &rtc_cfg);

        /* 7. Cleanup */
        av_capture_stop();
        for (int i = 0; i < MAX_SESSIONS; i++) {
            nano_session_destroy(&g_sessions[i]);
        }
        vt_encoder_destroy();
        opus_encoder_destroy(ctx.opus_enc);
        fq_destroy(&ctx.fq);
        http_sig_leave(&sig);

        fprintf(stderr, "Done.\n");
    }

    return 0;
}
