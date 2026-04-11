/*
 * frame_queue.c — Thread-safe encoded frame queue
 *
 * SPDX-License-Identifier: MIT
 */

#include "frame_queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

uint32_t g_fq_drop_count;

int fq_init(frame_queue_t *q)
{
    memset(q, 0, sizeof(*q));
    if (pipe(q->wake_pipe) < 0) {
        perror("pipe");
        return -1;
    }
    pthread_mutex_init(&q->lock, NULL);
    return 0;
}

void fq_destroy(frame_queue_t *q)
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

void fq_push(frame_queue_t *q, const uint8_t *data, size_t len,
             uint32_t pts_ms, bool is_keyframe)
{
    pthread_mutex_lock(&q->lock);
    if (q->count == FRAME_QUEUE_SIZE) {
        free(q->frames[q->head].data);
        q->head = (q->head + 1) % FRAME_QUEUE_SIZE;
        q->count--;
        g_fq_drop_count++;
    }
    int idx = (q->head + q->count) % FRAME_QUEUE_SIZE;
    q->frames[idx].data = malloc(len);
    if (q->frames[idx].data) {
        memcpy(q->frames[idx].data, data, len);
        q->frames[idx].len = len;
        q->frames[idx].pts_ms = pts_ms;
        q->frames[idx].is_keyframe = is_keyframe;
        q->count++;
    }
    pthread_mutex_unlock(&q->lock);

    char c = 'F';
    ssize_t w = write(q->wake_pipe[1], &c, 1);
    (void)w;
}

int fq_pop(frame_queue_t *q, queued_frame_t *out)
{
    pthread_mutex_lock(&q->lock);
    if (q->count == 0) {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    *out = q->frames[q->head];
    q->frames[q->head].data = NULL;
    q->head = (q->head + 1) % FRAME_QUEUE_SIZE;
    q->count--;
    pthread_mutex_unlock(&q->lock);
    return 0;
}
