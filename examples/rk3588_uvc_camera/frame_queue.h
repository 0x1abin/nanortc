/*
 * frame_queue.h — Thread-safe encoded frame queue
 *
 * Capture thread pushes H.264 frames; main event loop pops and
 * broadcasts to all connected viewers via nanortc.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FRAME_QUEUE_H_
#define FRAME_QUEUE_H_

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FRAME_QUEUE_SIZE 16

typedef struct {
    uint8_t *data;
    size_t len;
    uint32_t pts_ms;
    bool is_keyframe;
} queued_frame_t;

typedef struct {
    queued_frame_t frames[FRAME_QUEUE_SIZE];
    int head;
    int count;
    pthread_mutex_t lock;
    int wake_pipe[2]; /**< pipe[0]=read (for select), pipe[1]=write */
} frame_queue_t;

/** Diagnostic: number of frames dropped due to queue overflow. */
extern uint32_t g_fq_drop_count;

int  fq_init(frame_queue_t *q);
void fq_destroy(frame_queue_t *q);
void fq_push(frame_queue_t *q, const uint8_t *data, size_t len,
             uint32_t pts_ms, bool is_keyframe);
int  fq_pop(frame_queue_t *q, queued_frame_t *out);

#ifdef __cplusplus
}
#endif

#endif /* FRAME_QUEUE_H_ */
