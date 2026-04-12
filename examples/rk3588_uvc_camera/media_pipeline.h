/*
 * media_pipeline.h — Camera + microphone capture and session broadcast
 *
 * Owns the per-media queues and glues capture/audio backends to the
 * WebRTC sessions. The main event loop only needs to register the
 * pipeline's wake pipes into its fd_set and call drain_to_sessions
 * once per iteration; all kind-specific (video vs audio) branching
 * lives inside this module so main.c stays kind-agnostic.
 *
 * Video is always enabled. Audio is enabled iff @p aud_cfg is
 * non-NULL at init time AND the underlying audio_start() succeeds;
 * a failed audio start is non-fatal (logged, then silently
 * degrades to video-only).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MEDIA_PIPELINE_H_
#define MEDIA_PIPELINE_H_

#include "capture.h"
#include "media_queue.h"
#if RK3588_HAS_AUDIO
#include "audio_capture.h"
#endif
#include "multi_session.h"

#include <stdbool.h>
#include <stdint.h>
#include <sys/select.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    media_queue_t video_q;
#if RK3588_HAS_AUDIO
    media_queue_t audio_q;
#endif
    bool audio_enabled;

    /* Stats accumulators (reset by media_pipeline_take_*). */
    uint32_t frame_count;
    uint32_t bytes_sent;
    uint32_t idr_max_bytes;
} media_pipeline_t;

/**
 * @brief Start video capture and (optionally) audio capture.
 *
 * @param mp       Pipeline instance, zeroed on entry.
 * @param cap_cfg  Required video capture configuration.
 * @param aud_cfg  Audio configuration, or NULL for video-only builds.
 *                 Only meaningful when the binary was built with
 *                 @c RK3588_HAS_AUDIO.
 * @return 0 on success (video capture running; audio either running,
 *         disabled, or failed-but-non-fatal), -1 if video capture
 *         failed to start.
 */
#if RK3588_HAS_AUDIO
int  media_pipeline_init(media_pipeline_t *mp,
                         const capture_config_t *cap_cfg,
                         const audio_config_t *aud_cfg);
#else
int  media_pipeline_init(media_pipeline_t *mp,
                         const capture_config_t *cap_cfg);
#endif

/** @brief Stop all capture threads and release queues. */
void media_pipeline_shutdown(media_pipeline_t *mp);

/** @brief True if audio capture is running and feeding the audio queue. */
bool media_pipeline_audio_enabled(const media_pipeline_t *mp);

/**
 * @brief Register the queue wake pipes for select().
 *        Must be called each event-loop iteration before select().
 */
void media_pipeline_add_fds(media_pipeline_t *mp, fd_set *rset, int *maxfd);

/**
 * @brief Drain any ready queue and broadcast frames to every
 *        connected session. Fires nanortc_send_video /
 *        nanortc_send_audio and triggers per-session output dispatch.
 */
void media_pipeline_drain_to_sessions(media_pipeline_t *mp,
                                      const fd_set *rset,
                                      nano_session_t *sessions, int n_sessions,
                                      uint32_t *timeout_ms);

/* ----------------------------------------------------------------
 * Stats accessors — read + reset, so main.c's stats tick stays
 * state-free.
 * ---------------------------------------------------------------- */

uint32_t media_pipeline_take_frame_count(media_pipeline_t *mp);
uint32_t media_pipeline_take_bytes_sent(media_pipeline_t *mp);
uint32_t media_pipeline_take_idr_max_bytes(media_pipeline_t *mp);
uint32_t media_pipeline_take_video_drops(media_pipeline_t *mp);

#ifdef __cplusplus
}
#endif

#endif /* MEDIA_PIPELINE_H_ */
