/*
 * media_pipeline.c — Camera + microphone capture and session broadcast
 *
 * SPDX-License-Identifier: MIT
 */

#include "media_pipeline.h"
#include "nanortc.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#if RK3588_HAS_AUDIO
#include <pthread.h>
#include <signal.h>
#endif

#define VIDEO_QUEUE_CAPACITY 16
#define AUDIO_QUEUE_CAPACITY 32

uint32_t nano_get_millis(void); /* from run_loop_linux.c */

/* ----------------------------------------------------------------
 * Capture callbacks — run on producer threads.
 * ---------------------------------------------------------------- */

static void on_encoded_video(void *ctx, const uint8_t *annex_b, size_t len,
                             uint32_t pts_ms, bool is_keyframe)
{
    media_pipeline_t *mp = (media_pipeline_t *)ctx;
    media_queue_push(&mp->video_q, annex_b, len, pts_ms, is_keyframe);
}

#if RK3588_HAS_AUDIO
static void on_encoded_audio(void *ctx, const uint8_t *opus, size_t len,
                             uint32_t pts_ms)
{
    media_pipeline_t *mp = (media_pipeline_t *)ctx;
    media_queue_push(&mp->audio_q, opus, len, pts_ms, false);
}
#endif

/* ----------------------------------------------------------------
 * Init / shutdown
 * ---------------------------------------------------------------- */

#if RK3588_HAS_AUDIO
int media_pipeline_init(media_pipeline_t *mp,
                        const capture_config_t *cap_cfg,
                        const audio_config_t *aud_cfg)
#else
int media_pipeline_init(media_pipeline_t *mp,
                        const capture_config_t *cap_cfg)
#endif
{
    if (media_queue_init(&mp->video_q, MEDIA_KIND_VIDEO, VIDEO_QUEUE_CAPACITY) < 0)
        return -1;

#if RK3588_HAS_AUDIO
    if (media_queue_init(&mp->audio_q, MEDIA_KIND_AUDIO, AUDIO_QUEUE_CAPACITY) < 0) {
        media_queue_destroy(&mp->video_q);
        return -1;
    }
#endif

    capture_config_t cfg = *cap_cfg;
    cfg.callback = on_encoded_video;
    cfg.userdata = mp;
    if (capture_start(&cfg) < 0) {
#if RK3588_HAS_AUDIO
        media_queue_destroy(&mp->audio_q);
#endif
        media_queue_destroy(&mp->video_q);
        return -1;
    }

#if RK3588_HAS_AUDIO
    if (aud_cfg) {
        /* Block SIGUSR1 on the caller (main) thread so the audio
         * capture thread is the sole recipient — pthread_kill(SIGUSR1)
         * from audio_stop() wakes the blocking snd_pcm_readi() via
         * EINTR without disturbing the event loop. */
        sigset_t sigusr1;
        sigemptyset(&sigusr1);
        sigaddset(&sigusr1, SIGUSR1);
        pthread_sigmask(SIG_BLOCK, &sigusr1, NULL);

        audio_config_t acfg = *aud_cfg;
        acfg.callback = on_encoded_audio;
        acfg.userdata = mp;
        if (audio_start(&acfg) == 0) {
            mp->audio_enabled = true;
        } else {
            fprintf(stderr, "[audio] start failed, running video-only\n");
        }
    }
#endif
    return 0;
}

void media_pipeline_shutdown(media_pipeline_t *mp)
{
#if RK3588_HAS_AUDIO
    if (mp->audio_enabled) {
        audio_stop();
        mp->audio_enabled = false;
    }
#endif
    capture_stop();
#if RK3588_HAS_AUDIO
    media_queue_destroy(&mp->audio_q);
#endif
    media_queue_destroy(&mp->video_q);
}

bool media_pipeline_audio_enabled(const media_pipeline_t *mp)
{
    return mp->audio_enabled;
}

/* ----------------------------------------------------------------
 * fd_set integration
 * ---------------------------------------------------------------- */

void media_pipeline_add_fds(media_pipeline_t *mp, fd_set *rset, int *maxfd)
{
    int vfd = mp->video_q.wake_pipe[0];
    FD_SET(vfd, rset);
    if (vfd > *maxfd) *maxfd = vfd;
#if RK3588_HAS_AUDIO
    if (mp->audio_enabled) {
        int afd = mp->audio_q.wake_pipe[0];
        FD_SET(afd, rset);
        if (afd > *maxfd) *maxfd = afd;
    }
#endif
}

/* ----------------------------------------------------------------
 * Drain + broadcast
 * ---------------------------------------------------------------- */

static void drain_wake_pipe(int fd)
{
    char drain[64];
    ssize_t r = read(fd, drain, sizeof(drain));
    (void)r;
}

static void broadcast_video(media_pipeline_t *mp,
                            session_t *sessions, int n_sessions,
                            uint32_t *timeout_ms)
{
    media_frame_t frame;
    while (media_queue_pop(&mp->video_q, &frame) == 0) {
        bool sent = false;
        for (int i = 0; i < n_sessions; i++) {
            session_t *s = &sessions[i];
            if (!s->active || !s->media_connected || s->video_mid < 0) continue;
#if NANORTC_FEATURE_VIDEO
            nanortc_send_video(&s->rtc, (uint8_t)s->video_mid,
                               frame.pts_ms, frame.data, frame.len);
            session_dispatch_outputs(s, timeout_ms);
            sent = true;
#endif
        }
        if (sent) {
            mp->frame_count++;
            mp->bytes_sent += (uint32_t)frame.len;
            if (frame.is_keyframe && frame.len > mp->idr_max_bytes)
                mp->idr_max_bytes = (uint32_t)frame.len;
        }
        free(frame.data);
    }
}

#if RK3588_HAS_AUDIO
static void broadcast_audio(media_pipeline_t *mp,
                            session_t *sessions, int n_sessions,
                            uint32_t *timeout_ms)
{
    /* Rate-limited error reporter: many sends per second, but a
     * failing track must not flood stderr. */
    static uint32_t last_err_ms;
    static uint32_t err_count;

    media_frame_t frame;
    while (media_queue_pop(&mp->audio_q, &frame) == 0) {
        for (int i = 0; i < n_sessions; i++) {
            session_t *s = &sessions[i];
            if (!s->active || !s->media_connected || s->audio_mid < 0) continue;
#if NANORTC_FEATURE_AUDIO
            int rc = nanortc_send_audio(&s->rtc, (uint8_t)s->audio_mid,
                                        frame.pts_ms, frame.data, frame.len);
            if (rc != NANORTC_OK) {
                err_count++;
                uint32_t now = nano_get_millis();
                if (now - last_err_ms >= 2000) {
                    fprintf(stderr, "[audio] send_audio failed rc=%d mid=%d "
                                    "(count=%u in last 2s)\n",
                            rc, s->audio_mid, err_count);
                    last_err_ms = now;
                    err_count = 0;
                }
            }
            session_dispatch_outputs(s, timeout_ms);
#endif
        }
        free(frame.data);
    }
}
#endif

void media_pipeline_drain_to_sessions(media_pipeline_t *mp,
                                      const fd_set *rset,
                                      session_t *sessions, int n_sessions,
                                      uint32_t *timeout_ms)
{
    if (FD_ISSET(mp->video_q.wake_pipe[0], rset)) {
        drain_wake_pipe(mp->video_q.wake_pipe[0]);
        broadcast_video(mp, sessions, n_sessions, timeout_ms);
    }
#if RK3588_HAS_AUDIO
    if (mp->audio_enabled && FD_ISSET(mp->audio_q.wake_pipe[0], rset)) {
        drain_wake_pipe(mp->audio_q.wake_pipe[0]);
        broadcast_audio(mp, sessions, n_sessions, timeout_ms);
    }
#endif
}

/* ----------------------------------------------------------------
 * Stats
 * ---------------------------------------------------------------- */

uint32_t media_pipeline_take_frame_count(media_pipeline_t *mp)
{
    uint32_t v = mp->frame_count;
    mp->frame_count = 0;
    return v;
}

uint32_t media_pipeline_take_bytes_sent(media_pipeline_t *mp)
{
    uint32_t v = mp->bytes_sent;
    mp->bytes_sent = 0;
    return v;
}

uint32_t media_pipeline_take_idr_max_bytes(media_pipeline_t *mp)
{
    uint32_t v = mp->idr_max_bytes;
    mp->idr_max_bytes = 0;
    return v;
}

uint32_t media_pipeline_take_video_drops(media_pipeline_t *mp)
{
    uint32_t v = mp->video_q.drop_count;
    mp->video_q.drop_count = 0;
    return v;
}
