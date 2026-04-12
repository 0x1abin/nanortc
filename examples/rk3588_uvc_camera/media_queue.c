/*
 * media_queue.c — Thread-safe encoded media frame queue
 *
 * SPDX-License-Identifier: MIT
 */

#include "media_queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int media_queue_init(media_queue_t *q, media_kind_t kind, int capacity)
{
    memset(q, 0, sizeof(*q));
    if (capacity <= 0) return -1;
    q->slots = (media_frame_t *)calloc((size_t)capacity, sizeof(media_frame_t));
    if (!q->slots) return -1;
    if (pipe(q->wake_pipe) < 0) {
        free(q->slots);
        q->slots = NULL;
        perror("pipe");
        return -1;
    }
    q->kind = kind;
    q->capacity = capacity;
    pthread_mutex_init(&q->lock, NULL);
    return 0;
}

void media_queue_destroy(media_queue_t *q)
{
    if (!q->slots) return;
    pthread_mutex_lock(&q->lock);
    for (int i = 0; i < q->count; i++) {
        int idx = (q->head + i) % q->capacity;
        free(q->slots[idx].data);
    }
    pthread_mutex_unlock(&q->lock);
    pthread_mutex_destroy(&q->lock);
    close(q->wake_pipe[0]);
    close(q->wake_pipe[1]);
    free(q->slots);
    q->slots = NULL;
}

void media_queue_push(media_queue_t *q, const uint8_t *data, size_t len,
                      uint32_t pts_ms, bool is_keyframe)
{
    pthread_mutex_lock(&q->lock);
    if (q->count == q->capacity) {
        free(q->slots[q->head].data);
        q->head = (q->head + 1) % q->capacity;
        q->count--;
        q->drop_count++;
    }
    int idx = (q->head + q->count) % q->capacity;
    q->slots[idx].data = (uint8_t *)malloc(len);
    if (q->slots[idx].data) {
        memcpy(q->slots[idx].data, data, len);
        q->slots[idx].len = len;
        q->slots[idx].pts_ms = pts_ms;
        q->slots[idx].is_keyframe = is_keyframe;
        q->count++;
    }
    pthread_mutex_unlock(&q->lock);

    char c = (q->kind == MEDIA_KIND_VIDEO) ? 'V' : 'A';
    ssize_t w = write(q->wake_pipe[1], &c, 1);
    (void)w;
}

int media_queue_pop(media_queue_t *q, media_frame_t *out)
{
    pthread_mutex_lock(&q->lock);
    if (q->count == 0) {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    *out = q->slots[q->head];
    q->slots[q->head].data = NULL;
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    pthread_mutex_unlock(&q->lock);
    return 0;
}
