/*
 * sig_queue.c — Thread-safe signaling message queue
 *
 * SPDX-License-Identifier: MIT
 */

#include "sig_queue.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int sq_init(sig_queue_t *q)
{
    memset(q, 0, sizeof(*q));
    if (pipe(q->wake_pipe) < 0) {
        perror("pipe");
        return -1;
    }
    pthread_mutex_init(&q->lock, NULL);
    return 0;
}

void sq_destroy(sig_queue_t *q)
{
    pthread_mutex_destroy(&q->lock);
    close(q->wake_pipe[0]);
    close(q->wake_pipe[1]);
}

void sq_push(sig_queue_t *q, int msg_type, int from, const char *payload)
{
    pthread_mutex_lock(&q->lock);
    if (q->count == SIG_QUEUE_SIZE) {
        q->head = (q->head + 1) % SIG_QUEUE_SIZE;
        q->count--;
    }
    int idx = (q->head + q->count) % SIG_QUEUE_SIZE;
    q->msgs[idx].msg_type = msg_type;
    q->msgs[idx].from = from;
    size_t len = strlen(payload);
    if (len >= HTTP_SIG_BUF_SIZE) len = HTTP_SIG_BUF_SIZE - 1;
    memcpy(q->msgs[idx].payload, payload, len);
    q->msgs[idx].payload[len] = '\0';
    q->count++;
    pthread_mutex_unlock(&q->lock);

    char c = 'S';
    ssize_t w = write(q->wake_pipe[1], &c, 1);
    (void)w;
}

int sq_pop(sig_queue_t *q, sig_msg_t *out)
{
    pthread_mutex_lock(&q->lock);
    if (q->count == 0) {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    *out = q->msgs[q->head];
    q->head = (q->head + 1) % SIG_QUEUE_SIZE;
    q->count--;
    pthread_mutex_unlock(&q->lock);
    return 0;
}
