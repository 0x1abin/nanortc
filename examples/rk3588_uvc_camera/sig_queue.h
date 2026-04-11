/*
 * sig_queue.h — Thread-safe signaling message queue
 *
 * The signaling thread pushes incoming offers/candidates here;
 * the main event loop pops and processes them without blocking.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef SIG_QUEUE_H_
#define SIG_QUEUE_H_

#include "http_signaling.h"

#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SIG_QUEUE_SIZE 8

typedef struct {
    int msg_type; /**< 0 = offer, 1 = candidate */
    int from;     /**< viewer ID (from signaling server) */
    char payload[HTTP_SIG_BUF_SIZE];
} sig_msg_t;

typedef struct {
    sig_msg_t msgs[SIG_QUEUE_SIZE];
    int head;
    int count;
    pthread_mutex_t lock;
    int wake_pipe[2]; /**< pipe[0]=read (for select), pipe[1]=write */
} sig_queue_t;

int  sq_init(sig_queue_t *q);
void sq_destroy(sig_queue_t *q);
void sq_push(sig_queue_t *q, int msg_type, int from, const char *payload);
int  sq_pop(sig_queue_t *q, sig_msg_t *out);

#ifdef __cplusplus
}
#endif

#endif /* SIG_QUEUE_H_ */
